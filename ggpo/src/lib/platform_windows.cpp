/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "platform_windows.h"

int
Platform::GetConfigInt(LPCWSTR name)
{
	wchar_t buf[1024];
   if (GetEnvironmentVariable(name, buf, ARRAY_SIZE(buf)) == 0) {
      return 0;
   }
   return _wtoi(buf);
}

bool Platform::GetConfigBool(LPCWSTR name)
{
   wchar_t buf[1024];
   if (GetEnvironmentVariable(name, buf, ARRAY_SIZE(buf)) == 0) {
      return false;
   }
   return _wtoi(buf) != 0 || _wcsicmp(buf, L"true") == 0;
}
