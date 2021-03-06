/*
Copyright 2017 Microsoft
Permission is hereby granted, free of charge, to any person obtaining a copy of this software 
and associated documentation files (the "Software"), to deal in the Software without restriction, 
including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, 
subject to the following conditions:

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT 
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH 
THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "stdafx.h"
#include <vector>
#include <map>
#include "TimeCfg.h"
#include "..\SharedUtilities\Utils.h"
#include "..\SharedUtilities\TimeHelpers.h"
#include "..\SharedUtilities\JsonHelpers.h"
#include "..\SharedUtilities\Logger.h"
#include "..\SharedUtilities\DMException.h"
#include "..\SharedUtilities\PolicyHelper.h"
#include "CSPs\MdmProvision.h"
#include "ServiceManager.h"
#include "..\DMShared\ErrorCodes.h"

#include "Models\TimeInfo.h"

#define NtpServerPropertyName "NtpServer"

using namespace Windows::System;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::System::Profile;
using namespace Windows::Foundation::Collections;

using namespace Microsoft::Devices::Management::Message;

using namespace std;
using namespace Utils;

#ifdef GetObject
#undef GetObject
#endif

void TimeCfg::Get(TimeInfo& info)
{
    TRACE(__FUNCTION__);

    unsigned long returnCode;
    std::string output;
    Utils::LaunchProcess(L"C:\\windows\\system32\\w32tm.exe /query /configuration", returnCode, output);
    if (returnCode != 0)
    {
        throw DMExceptionWithErrorCode("Error: w32tm.exe returned an error code.", returnCode);
    }

    vector<string> lines;
    Utils::SplitString(output, '\n', lines);
    for (const string& line : lines)
    {
        TRACEP("Line: ", line.c_str());

        vector<string> tokens;
        Utils::SplitString(line, ':', tokens);
        if (tokens.size() == 2)
        {
            string name = Utils::TrimString(tokens[0], string(" "));
            string value = Utils::TrimString(tokens[1], string(" "));

            // remove the trailing " (Local)".
            size_t pos = value.find(" (Local)");
            if (pos != -1)
            {
                value = value.replace(pos, value.length() - pos, "");
            }

            TRACEP("--> Found: ", (name + " = " + value).c_str());
            if (name == NtpServerPropertyName)
            {
                info.ntpServer = MultibyteToWide(value.c_str());
            }
        }
    }

    // Local time
    info.localTime = MdmProvision::RunGetString(L"./DevDetail/Ext/Microsoft/LocalTime");

    // Time zone info...
    DYNAMIC_TIME_ZONE_INFORMATION tzi = { 0 };
    if (TIME_ZONE_ID_INVALID == GetDynamicTimeZoneInformation(&info.dynamicTimeZoneInformation))
    {
        throw DMExceptionWithErrorCode("Error: failed to retrieve time zone information.", GetLastError());
    }
}

void TimeCfg::Set(SetTimeInfoRequest^ setTimeInfoRequest)
{
    TRACE(__FUNCTION__);

    SetTimeInfoRequestData^ data = setTimeInfoRequest->data;

    SetNtpServer(data->ntpServer->Data());

    TRACEP("Bias: ", to_string(data->timeZoneBias).c_str());

    TRACEP("Standard Bias: ", to_string(data->timeZoneStandardBias).c_str());
    TRACEP(L"Standard Name: ", data->timeZoneStandardName->Data());
    TRACEP(L"Standard Date: ", data->timeZoneStandardDate->Data());
    TRACEP(L"Standard Day of Week: ", to_string(data->timeZoneStandardDayOfWeek).c_str());

    TRACEP("Daytime Bias: ", to_string(data->timeZoneDaylightBias).c_str());
    TRACEP(L"Daytime Name: ", data->timeZoneDaylightName->Data());
    TRACEP(L"Daytime Date: ", data->timeZoneDaylightDate->Data());
    TRACEP(L"Daytime Day of Week: ", to_string(data->timeZoneDaylightDayOfWeek).c_str());

    TRACEP(L"Zone Key Name: ", data->timeZoneKeyName->Data());
    TRACEP(L"Dynamic Daylight Time Disabled: ", to_string(data->dynamicDaylightTimeDisabled).c_str());

    DYNAMIC_TIME_ZONE_INFORMATION tzi = { 0 };

    // Use registry settings?
    tzi.DynamicDaylightTimeDisabled = data->dynamicDaylightTimeDisabled;

    // Option 1: If dynamicDaylightTimeDisabled = false, look-up the TimeZoneKeyName...
    wcsncpy_s(tzi.TimeZoneKeyName, data->timeZoneKeyName->Data(), _TRUNCATE);

    // Option 2: If dynamicDaylightTimeDisabled = true || timeZoneKeyName is not found, using the following time zone spec...

    // Bias...
    tzi.Bias = data->timeZoneBias;

    // Standard...
    wcsncpy_s(tzi.StandardName, data->timeZoneStandardName->Data(), _TRUNCATE);
    if (data->timeZoneStandardDate->Length() == 0)
    {
        // No support for daylight saving time.
        tzi.StandardDate.wMonth = 0;
    }
    else
    {
        if (!SystemTimeFromISO8601(data->timeZoneStandardDate->Data(), tzi.StandardDate))
        {
            throw DMExceptionWithErrorCode("Error: invalid date/time format.", GetLastError());
        }
    }
    tzi.StandardDate.wDayOfWeek = static_cast<WORD>(data->timeZoneStandardDayOfWeek);
    tzi.StandardBias = data->timeZoneStandardBias;

    // Daytime...
    wcsncpy_s(tzi.DaylightName, data->timeZoneDaylightName->Data(), _TRUNCATE);
    if (data->timeZoneDaylightDate->Length() == 0)
    {
        // No support for daylight saving time.
        tzi.DaylightDate.wMonth = 0;
    }
    else
    {
        if (!SystemTimeFromISO8601(data->timeZoneDaylightDate->Data(), tzi.DaylightDate))
        {
            throw DMExceptionWithErrorCode("Error: invalid date/time format.", GetLastError());
        }
    }
    tzi.DaylightDate.wDayOfWeek = static_cast<WORD>(data->timeZoneDaylightDayOfWeek);
    tzi.DaylightBias = data->timeZoneDaylightBias;

    // Set it...
    if (!SetDynamicTimeZoneInformation(&tzi))
    {
        throw DMExceptionWithErrorCode("Error: failed to set time zone information.", GetLastError());
    }

    TRACE(L"Time settings have been applied successfully.");
}

void TimeCfg::SetNtpServer(const std::wstring& ntpServer)
{
    TRACE(__FUNCTION__);
    TRACEP(L"New NTP Server = ", ntpServer.c_str());

    wstring command = L"";
    command += L"w32tm /config /manualpeerlist:";
    command += ntpServer;
    command += L" /syncfromflags:manual /reliable:yes /update";

    unsigned long returnCode = 0;
    std::string output;
    Utils::LaunchProcess(command, returnCode, output);
    if (returnCode != 0)
    {
        throw DMExceptionWithErrorCode("Error: w32tm.exe returned an error.", returnCode);
    }
}

GetTimeInfoResponse^ TimeCfg::Get()
{
    TRACE(__FUNCTION__);

    TimeInfo info;
    Get(info);

    GetTimeInfoResponseData^ data = ref new GetTimeInfoResponseData();
    data->localTime = ref new String(info.localTime.c_str());
    data->ntpServer = ref new String(info.ntpServer.c_str());

    data->dynamicDaylightTimeDisabled = info.dynamicTimeZoneInformation.DynamicDaylightTimeDisabled;
    data->timeZoneKeyName = ref new String(info.dynamicTimeZoneInformation.TimeZoneKeyName);

    data->timeZoneBias = info.dynamicTimeZoneInformation.Bias;

    data->timeZoneStandardName = ref new String(info.dynamicTimeZoneInformation.StandardName);
    data->timeZoneStandardDate = ref new String(Utils::ISO8601FromSystemTime(info.dynamicTimeZoneInformation.StandardDate).c_str());
    data->timeZoneStandardBias = info.dynamicTimeZoneInformation.StandardBias;
    data->timeZoneStandardDayOfWeek = info.dynamicTimeZoneInformation.StandardDate.wDayOfWeek;

    data->timeZoneDaylightName = ref new String(info.dynamicTimeZoneInformation.DaylightName);
    data->timeZoneDaylightDate = ref new String(Utils::ISO8601FromSystemTime(info.dynamicTimeZoneInformation.DaylightDate).c_str());
    data->timeZoneDaylightBias = info.dynamicTimeZoneInformation.DaylightBias;
    data->timeZoneDaylightDayOfWeek = info.dynamicTimeZoneInformation.DaylightDate.wDayOfWeek;

    return ref new GetTimeInfoResponse(ResponseStatus::Success, data);
}
