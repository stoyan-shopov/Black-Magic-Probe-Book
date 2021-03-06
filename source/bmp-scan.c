/*
 * Utility functions to scan for the Black Magic Probe on a system, and return
 * the (virtual) serial ports that it is assigned to. Under Microsoft Windows,
 * it scans the registry for the Black Magic Probe device, under Linux, it
 * browses through sysfs.
 *
 * Copyright 2019 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined WIN32 || defined _WIN32
  #define STRICT
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <tchar.h>
  #if defined __MINGW32__ || defined __MINGW64__
    #include <winreg.h>
    #define LSTATUS LONG
  #endif
#else
  #include <dirent.h>
  #include <unistd.h>
  #include <bsd/string.h>
#endif

#include "bmp-scan.h"


#if !defined sizearray
  #define sizearray(e)    (sizeof(e) / sizeof((e)[0]))
#endif
#if !defined MAX_PATH
  #define MAX_PATH    300
#endif


#if defined WIN32 || defined _WIN32

/** find_bmp() scans the system for the Black Magic Probe and a specific
 *  interface. For a serial interface, it returns the COM port and for the
 *  trace or DFU interfaces, it returns the GUID (needed to open a WinUSB
 *  handle on it).
 *  \param seqnr    The sequence number, must be 0 to find the first connected
 *                  device, 1 to find the second connected device, and so forth.
 *  \param iface    The interface number, e,g, BMP_IF_GDB for the GDB server.
 *  \param name     The COM-port name (or interface GUID) will be copied in
 *                  this parameter.
 *  \param namelen  The size of the "name" parameter (in characters).
 */
int find_bmp(int seqnr, int iface, TCHAR *name, size_t namelen)
{
  HKEY hkeySection, hkeyItem;
  TCHAR regpath[128];
  TCHAR subkey[128], portname[128], basename[128], *ptr;
  DWORD maxlen;
  int idx_device;
  BOOL found;

  assert(name != NULL);
  assert(namelen > 0);
  *name = '\0';

  /* find the device path for the GDB server */
  _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
            BMP_VID, BMP_PID, BMP_IF_GDB);
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
    return 0;

  /* Now we need to enumerate all the keys below the device path because more
     than a single BMP may have been connected to this computer.
     As we enumerate each sub-key we also check if it is the one currently
     connected */
  found = FALSE;
  idx_device = 0;
  while (!found && seqnr >= 0) {
    HKEY hkeySerialComm;
    int idx;
    /* find the sub-key */
    maxlen = sizearray(subkey);
    if (RegEnumKeyEx(hkeySection, idx_device, subkey, &maxlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0;
    }
    /* add the fixed portion & open the key to the item */
    _tcscat(subkey, "\\Device Parameters");
    if (RegOpenKeyEx(hkeySection, subkey, 0, KEY_READ, &hkeyItem) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0;
    }
    /* read the port name setting */
    maxlen = sizearray(portname);
    memset(portname, 0, maxlen);
    if (RegQueryValueEx(hkeyItem, _T("PortName"), NULL, NULL, (LPBYTE)portname, &maxlen) != ERROR_SUCCESS) {
      RegCloseKey(hkeyItem);
      RegCloseKey(hkeySection);
      return 0;
    }
    RegCloseKey(hkeyItem);
    /* clean up the port name and check that it looks correct (for a COM port) */
    if ((ptr = _tcsrchr(portname, _T('\\'))) != NULL)
      _tcscpy(basename, ptr + 1);     /* skip '\\.\', if present */
    else
      _tcscpy(basename, portname);
    for (idx = 0; basename[idx] != '\0' && (basename[idx] < '0' || basename[idx] > '9'); idx++)
      /* nothing */;
    if (basename[idx] == '\0') {  /* there is no digit in the port name, this can't be right */
      RegCloseKey(hkeySection);
      return 0;
    }

    /* check that the COM port exists (if it doesn't, portname is the "preferred"
       COM port for the Black Magic Probe, which is disconnected at the moment) */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("HARDWARE\\DEVICEMAP\\SERIALCOMM"), 0,
                    KEY_READ, &hkeySerialComm) != ERROR_SUCCESS) {
      RegCloseKey(hkeySection);
      return 0; /* no COM ports at all! */
    }
    for (idx = 0; !found; idx++) {
      TCHAR value[128];
      DWORD valsize;
      maxlen = sizearray(portname);
      valsize = sizearray(value);
      if (RegEnumValue(hkeySerialComm, idx, portname, &maxlen, NULL, NULL, (LPBYTE)value, &valsize) != ERROR_SUCCESS)
        break;
      if ((ptr = _tcsrchr(value, _T('\\'))) != NULL)
        ptr += 1;   /* skip '\\.\', if present */
      else
        ptr = value;
      if (_tcsicmp(ptr, basename) == 0 && seqnr-- == 0)
        found = TRUE;
    }
    RegCloseKey(hkeySerialComm);
    idx_device += 1;
  }
  RegCloseKey(hkeySection);

  if (!found)
    return 0;

  /* if we were querying for the port for GDB-server, the port name just found
     is also the one we need */
  if (iface == BMP_IF_GDB) {
    _tcsncpy(name, basename, namelen);
    name[namelen - 1] = '\0';
    return _tcslen(name) > 0;
  }

  /* otherwise, now open the key to the correct interface, and get a handle to
     the same subkey as the one for GDB-server */
  _stprintf(regpath, _T("SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_%04X&PID_%04X&MI_%02X"),
            BMP_VID, BMP_PID, iface);
  if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath, 0, KEY_READ, &hkeySection) != ERROR_SUCCESS)
    return 0;
  ptr = _tcschr(subkey, '\\');
  assert(ptr != NULL);
  *(ptr - 1) = (TCHAR)(iface + '0'); /* interface is encoded in the subkey too */
  if (RegOpenKeyEx(hkeySection, subkey, 0, KEY_READ, &hkeyItem) != ERROR_SUCCESS) {
    RegCloseKey(hkeySection);
    return 0;
  }
  maxlen = sizearray(portname);
  memset(portname, 0, maxlen);
  if (iface == BMP_IF_UART) {
    /* read the port name setting */
    if (RegQueryValueEx(hkeyItem, _T("PortName"), NULL, NULL, (LPBYTE)portname, &maxlen) != ERROR_SUCCESS) {
      RegCloseKey(hkeyItem);
      RegCloseKey(hkeySection);
      return 0;
    }
    if ((ptr = _tcsrchr(portname, _T('\\'))) != NULL)
      ptr += 1;     /* skip '\\.\', if present */
    else
      ptr = portname;
  } else {
    /* read GUID */
    LSTATUS stat = RegQueryValueEx(hkeyItem, _T("DeviceInterfaceGUIDs"), NULL, NULL, (LPBYTE)portname, &maxlen);
    /* ERROR_MORE_DATA is returned because there may technically be more GUIDs
       assigned to the device; we only care about the first one
       ERROR_FILE_NOT_FOUND is returned when the key is not found, which may
       happen on a clone BMP (without SWO trace support) */
    if (stat != ERROR_SUCCESS && stat != ERROR_MORE_DATA && stat != ERROR_FILE_NOT_FOUND) {
      RegCloseKey(hkeyItem);
      RegCloseKey(hkeySection);
      return 0;
    }
    ptr = portname;
  }
  RegCloseKey(hkeyItem);
  RegCloseKey(hkeySection);

  _tcsncpy(name, portname, namelen);
  name[namelen - 1] = '\0';
  return _tcslen(name) > 0;
}

#else

static int gethex(const char *ptr, int length)
{
  char hexstr[20];

  assert(ptr != NULL);
  assert(length > 0 && length < sizeof hexstr);
  memcpy(hexstr, ptr, length);
  hexstr[length] = '\0';
  return (int)strtol(hexstr, NULL, 16);
}

/** find_bmp() scans the system for the Black Magic Probe and a specific
 *  interface. For a serial interface, it returns the COM port and for the
 *  trace or DFU interfaces, it returns the GUID (needed to open a WinUSB
 *  handle on it).
 *  \param seqnr    The sequence number, must be 0 to find the first connected
 *                  device, 1 to find the second connected device, and so forth.
 *  \param iface    The interface number, e,g, BMP_IF_GDB for the GDB server.
 *  \param name     The COM-port name (or interface GUID) will be copied in
 *                  this parameter.
 *  \param namelen  The size of the "name" parameter (in characters).
 */
int find_bmp(int seqnr, int iface, char *name, size_t namelen)
{
  DIR *dsys;
  struct dirent *dir;

  assert(name != NULL);
  assert(namelen > 0);
  *name = '\0';

  /* run through directories in the sysfs branch */
  #define SYSFS_ROOT  "/sys/bus/usb/devices"
  dsys = opendir(SYSFS_ROOT);
  if (dsys == NULL)
    return 0;

  while (strlen(name) == 0 && seqnr >= 0 && (dir = readdir(dsys)) != NULL) {
    if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
      /* check the modalias file */
      char path[MAX_PATH];
      FILE *fp;
      sprintf(path, SYSFS_ROOT "/%s/modalias", dir->d_name);
      fp = fopen(path, "r");
      if (fp) {
        char str[256];
        memset(str, 0, sizeof str);
        fread(str, 1, sizeof str, fp);
        fclose(fp);
        if (memcmp(str, "usb:", 4) == 0) {
          const char *vid = strchr(str, 'v');
          const char *pid = strchr(str, 'p');
          const char *inf = strstr(str, "in");
          if (vid != NULL && gethex(vid + 1, 4) == BMP_VID
              && pid != NULL && gethex(pid + 1, 4) == BMP_PID
              && inf != NULL && gethex(inf + 2, 2) == BMP_IF_GDB)
          {
            DIR *ddev;
            /* tty directory this should be present for CDC ACM class devices */
            sprintf(path, SYSFS_ROOT "/%s/tty", dir->d_name);
            /* check the name of the subdirectory inside */
            ddev = opendir(path);
            if (ddev != NULL) {
              while (strlen(name) == 0 && (dir = readdir(ddev)) != NULL) {
                if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
                  if (seqnr-- == 0) {
                    strlcpy(name, "/dev/", namelen);
                    strlcat(name, dir->d_name, namelen);
                  }
                }
              }
              closedir(ddev);
            }
            if (strlen(name) > 0 && iface != BMP_IF_GDB) {
              /* GDB server was found for the requested sequence number,
                 but the requested interface is the UART or the SWO trace
                 interface -> patch the directory name and search again */
              char *ptr = path + strlen(path) - 5;  /* -4 for "/tty", -1 to get to the last character before "/tty" */
              assert(strlen(path) > 5);
              assert(*ptr == '0' && *(ptr-1) == '.' && *(ptr + 1) == '/');
              *ptr = iface + '0';
              *name = '\0'; /* clear device name for GDB-server (we want the name for the UART) */
              if (iface == BMP_IF_UART) {
                ddev = opendir(path);
                if (ddev != NULL) {
                  while (strlen(name) == 0 && (dir = readdir(ddev)) != NULL) {
                    if (dir->d_type == DT_LNK || (dir->d_type == DT_DIR && dir->d_name[0] != '.')) {
                      strlcpy(name, "/dev/", namelen);
                      strlcat(name, dir->d_name, namelen);
                    }
                  }
                  closedir(ddev);
                }
              } else {
                char *ptr = path + strlen(path) - 4;  /* -4 for "/tty" */
                assert(strlen(path) > 4);
                assert(*ptr == '/' && *(ptr - 1) == (iface + '0'));
                *ptr = '\0';  /* remove "/tty" */
                strlcat(path, "/modalias", sizearray(path));
                if (access(path, 0) == 0) {
                  /* file exists, so interface exists */
                  *ptr = '\0';  /* erase "/modalias" again */
                  ptr = path + strlen(SYSFS_ROOT) + 1;  /* skip root */
                  strlcpy(name, ptr, namelen);  /* return <bus> '-' <port> ':' <???> '.' <iface> */
                }
              }
            }
          }
        }
      }
    }
  }

  closedir(dsys);
  return strlen(name) > 0;
}

#endif

