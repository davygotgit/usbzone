//  Allow an end user to browse, load and save files to a USB drive. This avoids
//  the user inserting a USB drive directly to their computer. This may prevent
//  malware from spreading as the ESP32-P4 device will not automatically run
//  code.
//
//  This code inspired by ESP IDF v5.5.1 examples for SoftAP, the simple HTTP server
//  and USB MSC:
//
//      SoftAP: v5.5.1/esp-idf/examples/wifi/getting_started/softAP
//      HTTPD:  v5.5.1/esp-idf/examples/protocols/http_server/simple
//      MSC:    v5.5.1/esp-idf/examples/peripherals/usb/host/msc
//
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"

#define NULLCH              '\0'                            //  a NULL character

#define MAX_DIR_LEN         MAXNAMLEN                       //  max path/filename length
#define MAX_RESP_LEN        1024                            //  max HTTP response length
#define MAX_CHUNK_LEN       1024                            //  max chunk size for file transfers
#define BUFFER_PAD          64                              //  additional buffer overhead for HTML tags

#define STRINGIFY(x)        #x                              //  allows us to turn numbers into strings
#define TOSTRING(x)         STRINGIFY(x)

#define WIFI_SSID           "usbzone"                      //  WiFi configuration
#define WIFI_PASS           "topsecret"
#define WIFI_CHAN           6
#define WIFI_CONN           4   

#define MNT_PATH            "/usb"                          //  base mount path
#define MAX_MSC_DEVICES     1                               //  number of mounted USB devices allowed


//  Attribute to help free buffers, of various types, when they go out of scope
#define CLEANUP_CHAR        __attribute__((cleanup(FreeCharBuffer)))
#define CLEANUP_UCHAR       __attribute__((cleanup(FreeUCharBuffer)))
#define CLEANUP_WCHAR       __attribute__((cleanup(FreeWCharBuffer)))
#define CLEANUP_DIR         __attribute__((cleanup(CloseDIRHandle)))

static uint8_t                      USBAddr     = 0;        //  USB address
static msc_host_device_handle_t     USBHandle   = NULL;     //  USB device handle 
static msc_host_vfs_handle_t        VFSHandle   = NULL;     //  file system handle
static QueueHandle_t                eventQueue;             //  event queue for this application
struct timeval                      ourTime     = {0};      //  current time


//  Application event queue used to notify app_main() of
//  USB events
typedef struct
{
    enum
    {
        APP_DEVICE_CONNECTED,                               //  USB inserted event
        APP_DEVICE_DISCONNECTED,                            //  USB removed event
    } id;
    union
    {
        uint8_t                     deviceAddress;          //  address of inserted USB device
        msc_host_device_handle_t    deviceHandle;           //  USB device handle
    } data;
} app_message_t;

static const char* logTag = "usbshare";                     //  logging tag


//  WiFi Event Handler
static void WIFIEventHandler (void* arg, esp_event_base_t eventBase, int32_t eventID, void* eventData)
{
    if (eventID == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) eventData;
        ESP_LOGI(logTag, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else 
    if (eventID == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) eventData;
        ESP_LOGI(logTag, "station "MACSTR" leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
    }
}


//  Free a character buffer when the target variable goes out of scope
void FreeCharBuffer (char** buffer)
{
    free(*buffer);
}


//  Free an unsigned character buffer when the target variable goes out of scope
void FreeUCharBuffer (unsigned char** buffer)
{
    free(*buffer);
}


//  Free a wide character buffer when the target variable goes out of scope
void FreeWCharBuffer (wchar_t** buffer)
{
    free(*buffer);
}


//  Close a dirtectory handle when the target variable goes out of scope
void CloseDIRHandle (DIR** handle)
{
    if (*handle != NULL)
    {
        closedir(*handle);
    }
}


//  Common HTTP error handler
//
//  This gives the end user an HTML page with an error message
//
static esp_err_t ErrorHandler (httpd_req_t* req, const char* msg)
{
    char respString [MAX_RESP_LEN];
    snprintf(respString, sizeof(respString),    "<html><body><h1 style=\"text-align:center;\">Error!</h1>" \
                                                "<meta http-equiv=\"refresh\" content=\"5;url=index.html\">" \
                                                "<br>An error occurred. %s. The <a href=\"index.html\">main</a> page will reload in 5 seconds." \
                                                "</body></html>", msg);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, respString);
    return ESP_OK;
}


//  Root URL handler
static esp_err_t RootHandler (httpd_req_t* req)
{
    const char* respString =    "<html><body><h1 style=\"text-align:center;\">Welcome to the USB Safety and Sharing Zone</h1><p><br>Browse USB drives and transfer files without " \
                                "connecting the drive to your PC or network. Insert your USB drive into the device and select one of the following actions:" \
                                "<br><br><button style=\"width: 25%;\" onclick=\"window.location.href='info'\">USB Drive Information</button>" \
                                "<br><br><button style=\"width: 25%;\" onclick=\"window.location.href='load'\">Browse or load a file from USB Drive</button>" \
                                "<br><br><button style=\"width: 25%;\" onclick=\"window.location.href='save'\">Save a file to USB Drive</button>" \
                                "<br><br><button style=\"width: 25%;\" onclick=\"window.location.href='eject'\">Eject USB Drive</button></p></body></html>";

    httpd_resp_send(req, respString, strlen(respString));
    return ESP_OK;
}


//  Macro to handle some common error handler
#define CHECK_CHARS_WRITTEN(e)                                      \
{                                                                   \
    /*  Check characters written */                                 \
    if (charsWritten < 0)                                           \
    {                                                               \
        return ErrorHandler(req, e);                                \
    }                                                               \
                                                                    \
    /*  Keep a running total, so we don't overflow the buffer */    \
    totalChars += charsWritten;                                     \
}


//  USB Info URL handler
static esp_err_t InfoHandler (httpd_req_t* req)
{
    //  Make sure the USB drive was mounted
    if (USBHandle == NULL)
    {
        return ErrorHandler(req, "No information. USB drive not found");
    }

    //  Get the USB drive information
    msc_host_device_info_t info;
    ESP_ERROR_CHECK(msc_host_get_device_info(USBHandle, &info));

    //  Hold the USB drive output
    const size_t    infoSize                    = 2048;
    wchar_t*        infoBuffer CLEANUP_WCHAR    = (wchar_t *) malloc(infoSize * sizeof(wchar_t));

    //  Add initial HTML tags
    ssize_t totalChars      = 0;
    ssize_t charsWritten    = swprintf(infoBuffer, infoSize - 1,    L"<html><body><h1 style=\"text-align:center;\">USB Drive Information</h1><br><table>" \
                                                                    L"<tr><th>Attribute</th><th>Value</th</tr>");
    CHECK_CHARS_WRITTEN("USB drive information HTML tag issue");
        

    //  Add the capacity
    const size_t    MiB         = 1024 * 1024;
    const uint64_t  capacity    = ((uint64_t) info.sector_size * info.sector_count) / MiB;

    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Capacity</td><td>%llu MB</td></tr>", capacity);
    CHECK_CHARS_WRITTEN("USB drive capacity issue");

    //  Add the sector size
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Sector Size</td><td>%u</td></tr>", info.sector_size);
    CHECK_CHARS_WRITTEN("USB sector size issue");

    //  Add the sector count
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Sector Count</td><td>%u</td></tr>", info.sector_count);
    CHECK_CHARS_WRITTEN("USB sector count issue");

    //  Add the product ID
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Product ID</td><td>0x%04X</td></tr>", info.idProduct);
    CHECK_CHARS_WRITTEN("USB product ID issue");

    //  Add the vendor ID
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Vendor ID</td><td>0x%04X</td></tr>", info.idVendor);
    CHECK_CHARS_WRITTEN("USB vendor ID issue");

    //  Add the product name
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Product Name</td><td>%S</td></tr>", info.iProduct);
    CHECK_CHARS_WRITTEN("USB product name issue");

    //  Add the manufacturer 
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Manufacturer</td><td>%S</td></tr>", info.iManufacturer);
    CHECK_CHARS_WRITTEN("USB manufacturer issue");

    //  Add the serial number 
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"<tr><td>Serial Number</td><td>%S</td></tr>", info.iSerialNumber);
    CHECK_CHARS_WRITTEN("USB serial number issue");

    //  Add the closing HTML tags
    charsWritten = swprintf(&infoBuffer [totalChars], infoSize - totalChars - 1, L"</table><br>Return to the <a href=\"/\">main</a> page.</body></html>");
    CHECK_CHARS_WRITTEN("USB manufacturer issue");

    //  Now we need to convert the buffer to UTF-8
    const size_t UTFSize = totalChars * sizeof(wchar_t);

    //  Get the UTF-8 buffer
    char* UTFBuffer CLEANUP_CHAR = (char *) malloc(UTFSize + 1);
    if (UTFBuffer == NULL)
    {
        return ErrorHandler(req, "Could not allocate UTF-8");
    }

    //  Convert to UTF-8
    ssize_t UTFConv = wcstombs(UTFBuffer, infoBuffer, UTFSize + 1);
    if (UTFConv < 0)
    {
        return ErrorHandler(req, "Could not convert to UTF-8");
    }

    //  Set the response encoding and send the UTF-8 buffer
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req, UTFBuffer, UTFConv);

    return ESP_OK;
}


//  Let the user set some file system criteria like root directory to start
//  the search from etc.
static esp_err_t LoadHandler (httpd_req_t* req)
{
    //  Make sure the USB drive was mounted
    if (USBHandle == NULL)
    {
        return ErrorHandler(req, "No files. USB drive not found");
    }

    const char* respString =    "<html><body><h1 style=\"text-align:center;\">Load a File from the USB Drive</h1>" \
                                "<form action=\"select\" method=\"post\">" \
                                "<label for=\"startdir\">Starting directory:</label>" \
                                "<input type=\"text\" maxlength=\"32\" id=\"startdir\" name=\"startdir\" placeholder=\"/\">" \
                                "<br><br>" \
                                "<input type=\"checkbox\" id=\"recursive\" name=\"recursive\" value=\"1\">" \
                                "<label for=\"recursive\">Include subdirectory</label>" \
                                "<br><br>" \
                                "<label for=\"depth\">Max subdirectory depth:</label>&nbsp;" \
                                "<select id=\"depth\" name=\"depth\">" \
                                "<option value=\"1\">1</option>" \
                                "<option value=\"2\">2</option>" \
                                "<option value=\"3\">3</option>" \
                                "<option value=\"4\" selected>4</option>" \
                                "<option value=\"5\">5</option>" \
                                "<option value=\"6\">6</option>" \
                                "<option value=\"7\">7</option>" \
                                "<option value=\"8\">8</option>" \
                                "</select>" \
                                "<br><br>" \
                                "<input type=\"submit\" name=\"submit\" value=\"Submit\">&nbsp;" \
                                "<input type=\"reset\" name=\"reset\" value=\"Reset\">&nbsp;" \
                                "<input type=\"button\" name=\"cancel\" value=\"Cancel\" onclick=\"window.location.href='index.html';\">" \
                                "</form></body></html>";
    
    httpd_resp_send(req, respString, strlen(respString));
    return ESP_OK;
}


//  Return a pointer to the start of the POST data
static char* FindPostData (const char* data, const char* key)
{
    //  Append "=" to the key to reduce to possibility of a hit with data
    char newKey [BUFSIZ];
    snprintf(newKey, sizeof(newKey) - 1, "%s=", key);

    char* keyPtr = strstr(data, key);
    if (keyPtr == NULL)
    {
        //  Did not find the key
        return NULL;
    }

    //  Step over the key
    char* dataPtr = keyPtr + strlen(key) + 1;

    //  Return the pointer to the data
    return dataPtr;
}


//  Return the length of the POST data
static int FindPostDataLen (const char* data)
{
    char* endPtr = strchr(data, '&');
    if (endPtr == NULL)
    {
        //  Could be the end of the POST data which will
        //  be missing the & separator
        return strlen(data);
    }

    //  Return the length of the field
    return endPtr - data;
}


//  Find a field in the POST data
//
//  Post data will look like this:
//
//      startdir=&recursive=no&submit=Submit
//      startdir=%2F&recursive=no&submit=Submit
//
//  The data contains key value pairs separated by the & symbol
//
static bool ParsePostData (const char* data, const char* key, char* dest)
{
    char* srcData = FindPostData(data, key);
    if (srcData == NULL)
    {
        //  Did not find the data
        return false;
    }

    while (*srcData != NULLCH && *srcData != '&')
    {
        //  We might have a %XX sequence
        if (*srcData == '%')
        {
            //  Deal with the hex digit
            int digit;
            sscanf(srcData + 1, "%2x", &digit);
            *dest ++ = digit;

            //  Step over %XX
            srcData += 3;
        }
        else
        {
            //  Move the normal character
            *dest ++ = *srcData ++;
        }
    }

    //  Add a terminating NULL
    *dest = NULLCH;

    return true;
}


//  Common error handler for directory listing as we cannot
//  use the HTTP error code as we are in the middle of chunked
//  output. The best we can do is output an error message, as 
//  a chunk message, and carry on.
//
static bool ListError (httpd_req_t* req, const char* msg)
{
    //  Send the chunk and let the caller fail
    httpd_resp_send_chunk(req, msg, strlen(msg));
    return false;
}


//  See if a character string ends with a specific sequence
bool EndsWith (const char *string, const char *suffix)
{
    const size_t stringLen = strlen(string);
    const size_t suffixLen = strlen(suffix);

    //  Cannot continue if the suffix is longer than the string
    if (suffixLen > stringLen)
    {
        return false;
    }

    //  See if the string ends with the suffix
    if (memcmp(string + (stringLen - suffixLen), suffix, suffixLen) == 0)
    {
        //  Found it
        return true;
    }

    //  Not found
    return false;
}


//  Recursively list subdirectory contents
static bool ListDirectory (httpd_req_t* req, const char* baseDir, int depth, const int maxDepth)
{
    //	Stop if we have reached the maximum level requested
	if (depth != maxDepth
    &&  depth >= maxDepth)
	{
		return true;
	}

	//	See if we can open the directory
    const size_t    dirLen  = strlen(baseDir) + MAX_DIR_LEN;
    char*           dirPath = (char *) malloc(dirLen);
    if (dirPath == NULL)
    {
        return ListError(req, "Could not allocate initial directory");
    }

    //  Build up the correct path
    snprintf(dirPath, dirLen - 1, "%s%s", MNT_PATH, baseDir);

    ESP_LOGI(logTag, "Listing files in %s", dirPath);

    //  Open the directory
	DIR* dirHandle CLEANUP_DIR = opendir(dirPath);

    //  We don't need the directory path anymore
    free(dirPath);

    //  Any error?
	if (dirHandle == NULL)
	{
		return ListError(req, "Could not open a subdirectory");
	}

	//	Iterate through all files in the directory
	struct dirent* entry;
	while ((entry = readdir(dirHandle)) != NULL)
	{
        //  Every line needs a break
        const char* lineBreak = "<br>";
        httpd_resp_send_chunk(req, lineBreak, strlen(lineBreak));

		if (entry->d_type == DT_DIR) 
		{
			//	Skip current and previous directories
			if (strcmp(entry->d_name, ".") == 0
			||	strcmp(entry->d_name, "..") == 0)
			{
                continue;
			}
        }

        //  Output the indent
        const char* indent = "_&nbsp;";
        for (int i = 0; i < depth; i ++)
        {
            httpd_resp_send_chunk(req, indent, strlen(indent));
        }

		if (entry->d_type == DT_DIR) 
		{
			//	We need to create a new base directory
			const size_t    baseDirLen              = strlen(baseDir) + MAX_DIR_LEN + 1;
			char*           newBaseDir CLEANUP_CHAR = (char *) malloc(baseDirLen);
			if (newBaseDir == NULL)
			{
				printf("Could not allocate base directory\n");
				return false;
			}

            snprintf(newBaseDir, baseDirLen - 1, "%s%s", baseDir, entry->d_name);

            //  Append a trailing / if needed
            const size_t fullLen = strlen(newBaseDir);
            if (newBaseDir [fullLen] != '/')
            {
                strcat(newBaseDir, "/");
            }

            //  Now we need to create the output
            const size_t    outputLen           = MAX_DIR_LEN + BUFFER_PAD;
            char*           output CLEANUP_CHAR = (char *) malloc(outputLen);
            if (output == NULL)
            {
                ListError(req, "Could not allocate subdirectory");
                return false;
            }

            //  Directories are formatted as <dir>
            snprintf(output, outputLen - 1, "&lt;%s&gt;", entry->d_name);
            httpd_resp_send_chunk(req, output, strlen(output));

            //  Recursive requests will have a positive non zero maxDepth
            if (maxDepth > 0)
            {
                if (!ListDirectory(req, newBaseDir, depth + 1, maxDepth))
                {
                    ListError(req, "Could not list subdirectory");
                }
            }
		}
		else
		{
            //  Create a temporary buffer for the file information
            const size_t    outputLen           = strlen(baseDir) + (MAX_DIR_LEN * 3) + BUFFER_PAD;
            char*           output CLEANUP_CHAR = (char *) malloc(outputLen);
            if (output == NULL)
            {
                ListError(req, "Could not allocate output file");
            }

            //  See if we need to append a .usb to the filename. This occurs when PDF files are downloaded
            //  as browsers have a habbit of just opening the files, even through the Content-Disposition
            //  header has been set correctly
            //
            //  We may need to extend this list if other types of files are opened automatically
            //
            bool needsUSBExt    =   EndsWith(entry->d_name, ".pdf");
            needsUSBExt         |=  EndsWith(entry->d_name, ".PDF");

            //  Create the output
            snprintf(output, outputLen - 1, "<a href=\"/download?file=%s%s\" download=\"%s%s\">%s</a>", 
                        baseDir, entry->d_name,                         //  ?file=
                        entry->d_name, (needsUSBExt) ? ".usb" : "",     //  add .usb file extension if needed
                        entry->d_name);                                 //  link name

            //  Send the output
            httpd_resp_send_chunk(req, output, strlen(output));
		}
	}
	
    closedir(dirHandle);
    dirHandle = NULL;

	return true;
}


//  Produce a selectable list of files and subdirectory
static esp_err_t SelectHandler (httpd_req_t* req)
{
    //  Make sure the USB drive was mounted
    if (USBHandle == NULL)
    {
        return ErrorHandler(req, "Cannot select files. USB drive not found");
    }

    char        postData [BUFSIZ];
    const int   bytesRead = httpd_req_recv(req, postData, sizeof(postData));

    //  See if there is an error
    if (bytesRead <= 0)
    {
        return ErrorHandler(req, "Invalid file data received");
    }

    //  Add a NULL to the end of the data
    postData [bytesRead] = NULLCH;

    //  Process the form data
    ESP_LOGI(logTag, "File Select POST data: %s", postData);

    //  Get the starting directory
    char startDir [BUFFER_PAD];
    if (!ParsePostData(postData, "startdir", startDir))
    {
        return ErrorHandler(req, "Starting directory field missing");
    }

    //  Get the recursive search
    bool recursive = true;
    char recText [BUFFER_PAD];
    if (!ParsePostData(postData, "recursive", recText))
    {
        //  The recursive field will be missing if it was not checked
        recursive = false;
    }

    //  Default the starting directory if the user did not provide one
    if (startDir [0] == NULLCH)
    {
        strcpy(startDir, "/");
    }

    //  Get the subdirectory depth
    char depthText [BUFFER_PAD];
    if (!ParsePostData(postData, "depth", depthText))
    {
        //  The recursive field will be missing if it was not checked
        recursive = false;
    }
    int maxDepth = atoi(depthText);

    ESP_LOGI(logTag, "Recursive = %d, depth = %d\n", recursive, maxDepth);

    //  Send initial headers
    httpd_resp_set_type(req, "text/html");

    //  Output initial HTML page
    const char* startHTML = "<html><body><h1 style=\"text-align:center;\">Files on USB Drive</h1>";
    httpd_resp_send_chunk(req, startHTML, strlen(startHTML));

    //  List all files in the directory
    ListDirectory(req, startDir, 0, (recursive) ? maxDepth : 0);

    //  Output some useful links
    const char* linksHTML = "<br><br>Return to the <a href=\"/\">main</a> page, or <a href=\"/load\">look</a> for another file.";
    httpd_resp_send_chunk(req, linksHTML, strlen(linksHTML));

    //  Output end of HTML page
    const char* endHTML = "</body></html>";
    httpd_resp_send_chunk(req, endHTML, strlen(endHTML));
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}


//  Download a file
static esp_err_t DownloadHandler (httpd_req_t* req)
{
    //  Make sure the USB drive was mounted
    if (USBHandle == NULL)
    {
        return ErrorHandler(req, "Cannot download files. USB drive not found");
    }

    ESP_LOGI(logTag, "Received download request: %s", req->uri);

    //  We need to set up a buffer to extract the query which will
    //  be in GET format e.g. URL?file=name
    char URLFile [MAX_DIR_LEN];
    const size_t queryLen = httpd_req_get_url_query_len(req) + 1;
    if (queryLen > 1)
    {
        char* queryBuffer CLEANUP_CHAR = malloc(queryLen);
        if (queryBuffer == NULL)
        {
            return ErrorHandler(req, "Cannot allocate query buffer");
        }

        if (httpd_req_get_url_query_str(req, queryBuffer, queryLen) == ESP_OK)
        {
            if (httpd_query_key_value(queryBuffer, "file", URLFile, sizeof(URLFile)) != ESP_OK)
            {
                return ErrorHandler(req, "URL missing filename");
            }   
        }
        else
        {
            return ErrorHandler(req, "Could not copy URL information");
        }
    }
    else
    {
        return ErrorHandler(req, "Cannot determine URL length");
    }

    //  Create the filename on the USB drive
    const size_t    USBLen                  = MAX_DIR_LEN * 2;
    char*           USBFile CLEANUP_CHAR    = (char *) malloc(USBLen);
    if (USBFile == NULL)
    {
        return ErrorHandler(req, "Could not allocate USB filename");
    }

    snprintf(USBFile, USBLen - 1, "%s/%s", MNT_PATH, URLFile);

    //  Open the file
    FILE* handle = fopen(USBFile, "rb");
    if (handle == NULL)
    {
        return ErrorHandler(req, "Could not open file");
    }

    //  Send initial headers
    httpd_resp_set_type(req, "application/octet-stream");
    
    //  We need to read the file in chunks
    char* buffer CLEANUP_CHAR = (char *) malloc(MAX_CHUNK_LEN);
    if (buffer == NULL)
    {
        return ErrorHandler(req, "Could not allocate read chunk");
    }

    //  Loop until we hit the EOF
    while (!feof(handle))
    {
        //  Read the data
        size_t bytesRead = fread(buffer, 1, MAX_CHUNK_LEN, handle);
        if (bytesRead > 0)
        {
            //  Got data
            httpd_resp_send_chunk(req, buffer, bytesRead);
        }
        else
        {
            //  Could be an error or EOF
            break;
        }
    }

    //  No more data
    httpd_resp_send_chunk(req, NULL, 0);

    //  Make sure we hit the EOF
    bool EOFReached = feof(handle);

    //  Close the file
    fclose(handle);

    if (!EOFReached)
    {
       //  Some error condition rather than EOF
       return ErrorHandler(req, "Did not read enough of the file");
    }

    return ESP_OK;
}


//  Output the HTML to initiate a file save to the USB drive
static esp_err_t SaveHandler (httpd_req_t* req)
{
    //  Make sure the USB drive was mounted
    if (USBHandle == NULL)
    {
        return ErrorHandler(req, "Cannot save files. USB drive not found");
    }

    const char* respString =    "<html><body><h1 style=\"text-align:center;\">Save a File to the USB Drive</h1>" \
                                "<form id=\"sendform\">" \
                                "<input type=\"file\" id=\"sendfile\" name=\"sendfile\"><br><br>" \
                                "<input type=\"submit\" value=\"Submit\">&nbsp;" \
                                "<input type=\"reset\" value=\"Clear\">&nbsp;" \
                                "<input type=\"button\" name=\"cancel\" value=\"Cancel\" onclick=\"window.location.href='index.html';\">" \
                                "</form>" \
                                "<script>" \
                                "function EncodeTheData(input) " \
                                "{" \
                                "return new Promise((resolve, reject) =>" \
                                "{" \
                                "const reader = new FileReader();" \
                                "reader.onload = function()"\
                                "{" \
                                "const dataUrl = reader.result;" \
                                "const base64String = dataUrl.split(',')[1];" \
                                "resolve(base64String);" \
                                "};" \
                                "reader.onerror = function(error) " \
                                "{" \
                                "reject(error);" \
                                "};" \
                                "reader.readAsDataURL(input);" \
                                "});" \
                                "}" \
                                "const myForm = document.getElementById('sendform');" \
                                "myForm.addEventListener('submit', async (event) =>" \
                                "{" \
                                "event.preventDefault();" \
                                "const inputFile = myForm.elements.sendfile;" \
                                "if (inputFile.files.length != 1)" \
                                "{" \
                                "alert('Select one file');" \
                                "return;" \
                                "}" \
                                "const file = inputFile.files [0];" \
                                "const chunkSize = " TOSTRING(MAX_CHUNK_LEN) ";" \
                                "let remaining = file.size;" \
                                "let offset = 0;" \
                                "let index = 0;" \
                                "while (remaining > 0)" \
                                "{" \
                                "let thisChunk = Math.min(remaining, chunkSize);" \
                                "const dataChunk = file.slice(offset, offset + thisChunk);" \
                                "const encoded = await EncodeTheData(dataChunk);" \
                                "const timestamp = new Date();" \
                                "const formData = new FormData();" \
                                "formData.append('datachunk', encoded);" \
                                "formData.append('datalen', thisChunk);" \
                                "formData.append('chunksize', chunkSize);" \
                                "formData.append('filename', file.name);" \
                                "formData.append('index', index);" \
                                "formData.append('timestamp', timestamp.toISOString().replace(/\\.\\d{3}Z$/,'Z'));" \
                                "const encodedData = new URLSearchParams(formData).toString();" \
                                "try" \
                                "{" \
                                "const response = await fetch('/filechunk'," \
                                "{" \
                                "method: 'POST'," \
                                "headers: {'Content-Type': 'application/x-www-form-urlencoded'}," \
                                "body: encodedData" \
                                "});" \
                                "if (!response.ok)" \
                                "{" \
                                "throw new Error(`Transfer error, status: ${response.status}`);" \
                                "}" \
                                "}" \
                                "catch (error)" \
                                "{" \
                                "alert('Upload error');" \
                                "return;" \
                                "}" \
                                "remaining -= thisChunk;" \
                                "offset += thisChunk;" \
                                "index ++;" \
                                "}" \
                                "window.location.href='saved';"
                                "});" \
                                "</script></body></html>";

    httpd_resp_send(req, respString, strlen(respString));
    return ESP_OK;
}


//  Chunk error handler
static esp_err_t ChunkError (httpd_req_t* req, const char* msg)
{
    //  Output the error message
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_OK;
}


//  Accept a chunk of a file at a time
static esp_err_t ChunkHandler (httpd_req_t* req)
{
    //  Process the form data
    ESP_LOGI(logTag, "In the chunk handler");

    //  Make sure the USB drive was mounted
    if (USBHandle == NULL)
    {
        return ChunkError(req, "Cannot process chunk. USB drive not found");
    }

    char*       postData CLEANUP_CHAR   = (char *) malloc(MAX_CHUNK_LEN * 2);
    const int   bytesRead               = httpd_req_recv(req, postData, (MAX_CHUNK_LEN * 2) - 1);

    //  See if there is an error
    if (bytesRead <= 0)
    {
        return ChunkError(req, "Invalid chunk data received");
    }

    //  Add a NULL to the end of the data
    postData [bytesRead] = NULLCH;

    //  The POST data will be in the following format:
    //
    //      datachunk=W29iamVjdCBCbG9iXQ%3D%3D&datalen=13&chunksize=512&filename=TEST.TXT&index=0
    //
    //  We will get the datalen, chunksize, filename and index before the actual payload data
    //  in datachunk
    //

    ESP_LOGI(logTag, "Chunk data = %s\n", postData);
    
    //  Start with index
    char postField [BUFSIZ];
    if (!ParsePostData(postData, "index", postField))
    {
        return ChunkError(req, "Could not find chunk index");
    }
    const int index = atoi(postField);

    //  Get the chunk size
    if (!ParsePostData(postData, "chunksize", postField))
    {
        return ChunkError(req, "Could not find chunk size");
    }
    const int chunkSize = atoi(postField);

    //  Get the data length
    if (!ParsePostData(postData, "datalen", postField))
    {
        return ChunkError(req, "Could not find data length");
    }
    const int dataLen = atoi(postField);

    //  Get the filename
    char URLFile [BUFSIZ];
    if (!ParsePostData(postData, "filename", URLFile))
    {
        return ChunkError(req, "Could not find data filename");
    }

    //  Did we set the system time yet?
    if (ourTime.tv_sec == 0)
    {
        //  No - find the timestamp
        char timestamp [BUFSIZ];
        if (ParsePostData(postData, "timestamp", timestamp))
        {
            //  We got a date and time field.
            //
            //  Note:   We don't throw an error if we are missing
            //          this field as the only downside is a 
            //          bad file creation time
            struct tm timeInfo;

            ESP_LOGI(logTag, "Attempt to set date and time from %s", timestamp);

            //  Make sure we can parse the date information
            //2025-11-03T03:30:04.091Z
            if (strptime(timestamp, "%Y-%m-%dT%H:%M:%S%Z", &timeInfo) == NULL
            &&  strptime(timestamp, "%Y-%m-%dT%H:%M:%S%z", &timeInfo) == NULL)
            {
                //  Could not parse the timestamp, but we can continue
                ESP_LOGW(logTag, "Could not parse date and time from %s", timestamp);
            }
            else
            {
                //  We need the the number of seconds since epoch
                time_t seconds = mktime(&timeInfo);
                if (seconds != (time_t) -1)
                {
                    //  Set system time
                    ourTime.tv_sec = seconds;
                    settimeofday(&ourTime, NULL);
                }
                else
                {
                    ESP_LOGW(logTag, "Could not convert timestamp to seconds");
                }
            }
        }
        else
        {
            ESP_LOGW(logTag, "Missing timestamp field");
        }
    }

    //  Now we can get the data. We will start by finding the pointer
    //  to the start of the datachunk field
    char* srcData = FindPostData (postData, "datachunk");
    if (srcData == NULL)
    {
        return ChunkError(req, "Could not find data chunk");
    }

    //  Get the length of this field
    size_t srcLen = FindPostDataLen(srcData);

    //  Only process fields with data
    if (srcLen > 0)
    {
        //  The source data might have %3D instead of an =. We need to parse the data
        //  and clean out any %XX sequences or the base64 decode won't work
        char* b64Data CLEANUP_CHAR = (char *) malloc(srcLen);
        if (b64Data == NULL)
        {
            return ChunkError(req, "Could not allocate chunk source");
        }

        //  Parse the data
        if (!ParsePostData(postData, "datachunk", b64Data))
        {
            return ChunkError(req, "Could not convert data chunk");
        }

        //  Reset the source data length. If we removed %3D, the input
        //  will be smaller. Also, the data must be a multiple of 4
        srcLen = strlen(b64Data);
        if (srcLen % 4 != 0)
        {
            return ChunkError(req, "Data chunk incorrect length");
        }

        //  Determine the length of the chunk
        size_t destLen = 0;
        mbedtls_base64_decode(NULL, 0, &destLen, (unsigned char *) b64Data, srcLen);
        
        //  Only positive non-zero decoded length are good
        if (destLen <= 0)
        {
            return ChunkError(req, "Decoded length is <= 0");
        }

        //  This better match our payload
        if (destLen != dataLen)
        {
            return ChunkError(req, "Decoded length does not match payload length");
        }

        //  Allocate a buffer
        unsigned char* destData CLEANUP_UCHAR = (unsigned char *) malloc(destLen + 1);
        if (destData == NULL)
        {
            return ChunkError(req, "Could not allocate destination data");
        }

        //  Decode the buffer
        size_t decodedLen;
        if (mbedtls_base64_decode(destData, destLen, &decodedLen, (unsigned char *) b64Data, srcLen))
        {
            return ChunkError(req, "Unable to decode data");
        }

        //  We have to create the correct file path on the USB drive
        size_t  USBLen                  = strlen(URLFile) + MAX_DIR_LEN;
        char*   USBFile CLEANUP_CHAR    = (char *) malloc(USBLen);
        if (USBFile == NULL)
        {
            return ChunkError(req, "Could not allocate initial directory");
        }   

        //  Build up the correct path
        snprintf(USBFile, USBLen - 1, "%s/%s", MNT_PATH, URLFile);

        ESP_LOGI(logTag, "USB file is %s", USBFile);

        //  Determine if we will create or append to the file
        const char* access = (index == 0) ? "wb" : "ab";

        //  Open the file
        FILE* chunkHandle = fopen(USBFile, access);
        if (chunkHandle == NULL)
        {
            return ChunkError(req, "Could not create save file");
        }

        //  Move the file pointer to the correct position
        const size_t filePos = index * chunkSize;

        ESP_LOGI(logTag, "File position is %d\n", filePos);

        fseek(chunkHandle, filePos, SEEK_SET);

        //  Write the buffer
        const size_t bytesWritten = fwrite(destData, 1, destLen, chunkHandle);

        //  Done with the file
        fflush(chunkHandle);
        fclose(chunkHandle);

        //  Might have been a write error
        if (bytesWritten != destLen)
        {
            return ChunkError(req, "Did not write enough data to USB drive");
        }
    }

    //  Send a good response
    const char* goodResp = "OK";
    httpd_resp_send(req, goodResp, strlen(goodResp));

    return ESP_OK;
}


//  File saved URL handler
static esp_err_t SavedHandler (httpd_req_t* req)
{

    const char* respString =    "<html><body><h1 style=\"text-align:center;\">File Saved to USB Drive</h1>" \
                                "<meta http-equiv=\"refresh\" content=\"5;url=/\">" \
                                "<br>The file was saved to the USB drive. The <a href=\"/\">main</a> page will reload in 5 seconds." \
                                "</body></html>";

    httpd_resp_send(req, respString, strlen(respString));
    return ESP_OK;
}


//  Eject USB URL handler
//
//  Note:   Additional cleanup code, before the USB drive is removed can go here
//
static esp_err_t EjectHandler ( httpd_req_t* req)
{
    const char* respString =    "<html><body><h1 style=\"text-align:center;\">Safe to remove USB Drive</h1>" \
                                "<meta http-equiv=\"refresh\" content=\"5;url=/\">" \
                                "<br>You can now remove the USB drive. The <a href=\"/\">main</a> page will reload in 5 seconds." \
                                "</body></html>";

    httpd_resp_send(req, respString, strlen(respString));
    return ESP_OK;
}


httpd_handle_t InitHTTP (void)
{
    ESP_LOGI(logTag, "Start HTTP Server");

    //  Start the HTTP server
    httpd_handle_t HTTPServer = NULL;
    httpd_config_t HTTPConfig = HTTPD_DEFAULT_CONFIG();

    //  Make sure we can handle all the URLs
    HTTPConfig.max_uri_handlers = 16;

    //  Configure the HTTP server
    ESP_ERROR_CHECK(httpd_start(&HTTPServer, &HTTPConfig));

    //  Create URL handlers
    httpd_uri_t rootURL     =   {.uri = "/",                .method = HTTP_GET,     .handler = RootHandler,     .user_ctx  = NULL};
    httpd_uri_t indexURL    =   {.uri = "/index.html",      .method = HTTP_GET,     .handler = RootHandler,     .user_ctx  = NULL};
    httpd_uri_t infoURL     =   {.uri = "/info",            .method = HTTP_GET,     .handler = InfoHandler,     .user_ctx  = NULL};
    httpd_uri_t loadURL     =   {.uri = "/load",            .method = HTTP_GET,     .handler = LoadHandler,     .user_ctx  = NULL};
    httpd_uri_t selectURL   =   {.uri = "/select",          .method = HTTP_POST,    .handler = SelectHandler,   .user_ctx  = NULL};
    httpd_uri_t downloadURL =   {.uri = "/download",        .method = HTTP_GET,     .handler = DownloadHandler, .user_ctx  = NULL};
    httpd_uri_t uploadURL   =   {.uri = "/save",            .method = HTTP_GET,     .handler = SaveHandler,     .user_ctx  = NULL};
    httpd_uri_t chunkURL    =   {.uri = "/filechunk",       .method = HTTP_POST,    .handler = ChunkHandler,    .user_ctx  = NULL};
    httpd_uri_t savedURL    =   {.uri = "/saved",           .method = HTTP_GET,     .handler = SavedHandler,    .user_ctx  = NULL};
    httpd_uri_t ejectURL    =   {.uri = "/eject",           .method = HTTP_GET,     .handler = EjectHandler,    .user_ctx  = NULL};

    //  Register URL handlers
    httpd_register_uri_handler(HTTPServer, &rootURL);
    httpd_register_uri_handler(HTTPServer, &indexURL);
    httpd_register_uri_handler(HTTPServer, &infoURL);
    httpd_register_uri_handler(HTTPServer, &loadURL);
    httpd_register_uri_handler(HTTPServer, &selectURL);
    httpd_register_uri_handler(HTTPServer, &downloadURL);
    httpd_register_uri_handler(HTTPServer, &uploadURL);
    httpd_register_uri_handler(HTTPServer, &chunkURL);
    httpd_register_uri_handler(HTTPServer, &savedURL);
    httpd_register_uri_handler(HTTPServer, &ejectURL);

    ESP_LOGI(logTag, "HTTP Server started");

    return HTTPServer;
}


//  Initialize the access point the user will attach to
void InitAP (void)
{
    ESP_LOGI(logTag, "Start AP");

    //  Verify the NW layer can initializa
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    //  Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    //  Add WiFi event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WIFIEventHandler,
                                                        NULL,
                                                        NULL));

    //  Create AP configuration
    wifi_config_t wifiConfig =
    {
        .ap = 
        {
            .ssid               = WIFI_SSID,
            .ssid_len           = strlen(WIFI_SSID),
            .channel            = WIFI_CHAN,
            .password           = WIFI_PASS,
            .max_connection     = WIFI_CONN,
            .authmode           = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg            = { .required = true, },    //  use Protected Management Frames
            .gtk_rekey_interval = 1800,                     //  new Group Temporal Key every 30 minutes
        },
    };

    if (strlen(WIFI_PASS) == 0) 
    {
        wifiConfig.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(logTag, "WiFi AP started. SSID: %s channel: %d", WIFI_SSID, WIFI_CHAN);
}


//  Allocate a new USB device
static esp_err_t AllocateUSBDevice (const app_message_t* msg)
{
    esp_err_t err = msc_host_install_device(msg->data.deviceAddress, &USBHandle);
    if (err != ESP_OK)
    {
        ESP_LOGE(logTag, "Unable to allocate new USB device: %s", esp_err_to_name(err));
        return err;
    }

    //  Set the USB address
    USBAddr = msg->data.deviceAddress;

    //  Mount the file system
    const esp_vfs_fat_mount_config_t mountConfig =
    {
        .format_if_mount_failed = false,
        .max_files              = 3,
        .allocation_unit_size   = 8192,
    };

    //  Set the local mount point e.g. /usb
    char mountPath[16];
    strcpy(mountPath, MNT_PATH);

    err = msc_host_vfs_register(USBHandle, mountPath, &mountConfig, &VFSHandle);
    if (err != ESP_OK)
    {
        //  Report the error
        ESP_LOGE(logTag, "Unable to mount file system: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(msc_host_uninstall_device(USBHandle));

        //  Clear handles etc. to the failed mount
        USBAddr     = 0;
        USBHandle   = NULL;
        VFSHandle   = NULL;
        return err;
    }

    ESP_LOGI(logTag, "USB drive mounted to %s", MNT_PATH);

    return ESP_OK;
}


//  Free the resources of the USB device
static void FreeUSBDevice (void)
{
    //  Unmount the filesystem
    if (VFSHandle) 
    {
        ESP_ERROR_CHECK(msc_host_vfs_unregister(VFSHandle));
    }
    
    //  Remove the USB device
    if (USBHandle) 
    {
        ESP_ERROR_CHECK(msc_host_uninstall_device(USBHandle));
    }

    //  Reset the USB address
    USBAddr = 0;
}


//  USB callback to handle connect and disconnect events
static void USBEventCallback (const msc_host_event_t* event, void* arg)
{
    if (event->event == MSC_DEVICE_CONNECTED) 
    {
        ESP_LOGI(logTag, "USB device connected (USB Address %d)", event->device.address);

        //  Create the connect message
        app_message_t message = 
        {
            .id                 = APP_DEVICE_CONNECTED,
            .data.deviceAddress = event->device.address,
        };
        
        //  Send to app_main()
        xQueueSend(eventQueue, &message, portMAX_DELAY);
    } 
    else 
    if (event->event == MSC_DEVICE_DISCONNECTED) 
    {
        if (event->device.handle == USBHandle)
        {
            ESP_LOGI(logTag, "USB device disconnected (USB Address %d)", USBAddr);

        }
        else
        {
            //  FIXME:  This should probably communicate this issue to app_main() 
            ESP_LOGW(logTag, "Unexpected USB device disconnected");
        }

        //  Create the disconnect message
        app_message_t message =
        {
            .id                 = APP_DEVICE_DISCONNECTED,
            .data.deviceHandle  = event->device.handle,
        };
        
        //  Send to app_main()
        xQueueSend(eventQueue, &message, portMAX_DELAY);
    }
}


//  Load various parts of the USB stack
static void USBTask (void* args)
{
    //  Load the USB host handler
    const usb_host_config_t hostConfig = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&hostConfig));

    //  Load the mass storage handler
    const msc_host_driver_config_t massStorageConfig = {
        .create_backround_task  = true,
        .task_priority          = 5,
        .stack_size             = 4096,
        .callback               = USBEventCallback,

    };
    ESP_ERROR_CHECK(msc_host_install(&massStorageConfig));

    //  This loop should run forever, but we leave the demo
    //  code to clean up the USB stack if the loop ever
    //  exits
    bool hasClients = true;
    while (true)
    {
        uint32_t eventFlags;
        usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);

        // Release devices once all clients has deregistered
        if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) 
        {
            hasClients = false;
            if (usb_host_device_free_all() == ESP_OK) 
            {
                break;
            };
        }

        if (eventFlags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE 
        &&  !hasClients) 
        {
            break;
        }
    }

    //  Short delay to allow clients to unload
    vTaskDelay(10);

    //  Rest of the cleanup
    ESP_LOGI(logTag, "Stopping USB");
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}


void app_main (void)
{
    //  Initialize NVS
    esp_err_t NVSStatus = nvs_flash_init();
    if (NVSStatus == ESP_ERR_NVS_NO_FREE_PAGES 
    ||  NVSStatus == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        NVSStatus = nvs_flash_init();
    }
    ESP_ERROR_CHECK(NVSStatus);

    //  Initialize the AP
    InitAP();

    //  Initialize the HTTP server
    httpd_handle_t HTTPServer = InitHTTP();

    //  The HTTP handle should not be NULL
    if (HTTPServer == NULL)
    {
        ESP_LOGE(logTag, "HTTP Server did not start correctly");
        while(true)
        {
            vTaskDelay(1000);
        }
    }

    //  Create request queue
    eventQueue = xQueueCreate(5, sizeof(app_message_t));
    assert(eventQueue);

    //  Initialize the USB stack
    BaseType_t task_created = xTaskCreate(USBTask, "USBTask", 4096, NULL, 2, NULL);
    assert(task_created);

    //  Loop forever
    while (true)
    {
        app_message_t msg;
        xQueueReceive(eventQueue, &msg, portMAX_DELAY);

        if (msg.id == APP_DEVICE_CONNECTED) 
        {
            //  A new device connected - allocate USB resources
            esp_err_t res = AllocateUSBDevice(&msg);
            if (res != ESP_OK)
            {
                ESP_LOGI(logTag, "USB device failed to allocated");
                continue;
            }

        }
        else
        if (msg.id == APP_DEVICE_DISCONNECTED)
        {
            //  A drive disconnected - free resources
            FreeUSBDevice();
        }
    }
}
