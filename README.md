# usbzone
USB Safety and Sharing Zone

## Background
You just purchased an off-brand USB drive, or you found a stray USB drive. Are you condifent enough to plug the USB drive into your laptop, desktop or device (smartphone/tablet)? USB drives are easy to use and a convenient way to share files, and malware.

## The Concept
The following diagram describes the concept driving this project:

<img width="50%" height="50%" alt="8" src="https://github.com/user-attachments/assets/ee41fc1b-c655-4c0c-b3ef-e08cf1907c1f" />

The USB drive is inserted into an intermediate device that is separated from any laptop, desktop or smartphone/tablet. The intermediate device has a limited functionality OS which reduces the attack surface. Laptops, etc., connect to the intermediate device to upload or download files. Downloaded files can be assessed by local anti-virus software before being used.

Here's a more detailed diagram:

<img width="50%" height="50%" alt="7" src="https://github.com/user-attachments/assets/4fb3413b-4a89-4516-9cc1-99d33a82a86b" />

The intermediate device, in this case an ESP32-P4 Nano, creates a WiFi access point with a HTTP server. The end user device, which could be a laptop, connects to the access point and uses a browser to interact with the HTTP server. The web interface allows the end user to browse files on the USB drive as well as download and upload files.

## The Project in Action
The application presents a minimalist web interface that provides the functionality required, and minimizes memory overhead on the ESP32-P4 Nano. The UI/UX could be improved with a minor increase in memory usage.

The following sections give a feel for the web UI/UX.

### The Main Page
The following screenshot shows the project’s main web page served by the ESP32-P4 Nano:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/72cae863-c159-49ec-99c1-fc224a6454b5" />

### USB Information
Clicking the USB Drive Information button results in the following page:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/cf5385d7-5185-44c1-b593-48276b74dec0" />

This page outputs information about the USB drive like the capacity, and product model and manufacturer. The user can return to the main page by clicking on the link at the bottom of the page.

### Browsing and Loading Files
Clicking on the Browse or load a file from USB Drive button displays this initial page:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/53976290-9c3c-4a95-bc8f-2c2a0eaa67cc" />

The end user can specify a starting directory, whether to include subdirectories (recursive) and the maximum subdirectory depth. 

Selecting the defaults produces the following page:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/52a0e253-3b1b-4657-9663-773319ab59cd" />

This page gives a list of files on the USB drive that can be reviewed selected for download. The end user can also choose to return to the main page or perform another search.

Selecting the usbtest.pdf file causes a file called usbtest.pdf.usb to be downloaded. This is a little inconvenient, as the end user has to rename the PDF file, but this prevents some browsers automatically opening the file. Opening the PDF file must be avoided as it could contain a malicious payload.

There is a HTTP header called Content-Disposition that can be set to indicate a file should not be opened, but this seems to be ignored, and PDF files are opened based on the file extension.

The downloaded usbtest.pdf.usb file matches the source version of the file:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/7537507d-60bb-4ff3-b181-a46c336d8bc9" />

### Saving Files
Clicking on the Save a File to USB drive button produces the following page:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/a36eff92-bd3a-4dc9-aebf-70c66136b211" />

The end user can select a file to upload to the USB drive:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/c50dbb43-0e55-41cc-91ea-567f67309bb1" />

After selection, the end user can submit the file to the ESP32-P4 Nano:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/dd27994b-a8f5-4f2b-8170-f3ce2aa703f2" />

The end user will see the following confirmation page before being returned to the main page:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/4c3204d1-7e8e-4e03-bc02-cc6d23ae1a79" />

The file was transferred to the USB drive:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/3b5f3d2d-c66e-4214-8ad9-9473cbc5d086" />

### Eject
Clicking on the Eject USB Drive button produces the following page:

<img width="50%" height="50%" alt="image" src="https://github.com/user-attachments/assets/46bbabd2-343d-4425-b7b7-c95da5ebce6f" />

The end user can remove the USB drive at any time, but this option gives the end user the option of advertising their intent which allows the ESP32-P4 Nano to perform additional cleanup.

## How to Build the Project
The project requires VSCode and the ESP-IDF Framework. The ESP32-P4 Nano has a great Wiki page here https://www.waveshare.com/wiki/ESP32-P4-Nano-StartPage and a section on installing VSCode and the ESP-IDF Framework here https://www.waveshare.com/wiki/ESP32-P4-Nano-StartPage#ESP-IDF. 

Once VSCode and the ESP-IDF Framework are installed, the following command to download the project's repository:

  git clone https://github.com/davygotgit/usbzone.git

This should create a local folder called usbzone, which you can use VSCode to open e.g. File -> Open Folder. The project can then be built and flashed to the ESP32-P4 Nano.

## What Challenges Did You Face?
I was not familiar with either VSCode or the ESP-IDF Framework. I typically use Arduino IDE when working with microcontrollers. However, the Arduino IDE environment was missing libraries to access the USB drive and file system. You will run into challenges when you start using a new environment. As the VSCode UI/UX is close to Visual Studio, which I have used extensively, I was able to adapt to the IDE quickly. 

I ran into one challenge with ESP-IDF. I was not aware that projects were configured using a combination of CmakeLists.txt and idf_component.yml to identify additional components like HTTP servers and WiFi, as well as menuconfig to configure those components.

The main programming challenge I faced was with the save web page. For this page to work, there is cooperation between an HTML form, some JavaScript and the filechunk URL handler, the ChunkHandler() function, on the ESP32-P4 Nano. The HTML form is used to identify a file to upload. The JavaScript to splits the file into chunks, as an entire file cannot be sent in one go. The ChunkHandler() function reads chunks of data and assembles them into a single file.

Having implemented this type of functionality in another project with an HTML page, Nginx and PHP, I started with an HTML form that used multipart/form-data encoding. Although this did send data in chunks, the POST data was in format that would be required extra code to parse. I had already written code to parse POST data in URL format e.g. key1=value1&key2=value2&keyn=valuen. I decided I wanted to reuse this parsing code, which introduced a complication when sending and writing binary data.

The JavaScript code to determine how much data to send, is straight forward:

	let thisChunk = Math.min(remaining, chunkSize);
	const dataChunk = file.slice(offset, offset + thisChunk);

At this point in the code, the file has been opened, the chunkSize is 1024 bytes which means dataChunk variable has a JavaScript Blob object containing 1024 bytes. As I don’t do a lot of JavaScript coding, I originally Base64 encoded the dataChunk variable, but this resulted in file contents that just read [object Blob] when inspected. I was not aware that the Blob data had to be read and the odd contents I was seeing was just a default string from the Blob object. After some research, I found some code that read the contents of the Blob and converted it to Base64 encoding:

	const encoded = await EncodeTheData(dataChunk);

The EncodeTheData() function is included in the project files, but not shown here. The encoded variable now has the correct Base64 encoded data.

The JavaScript continues by creating a dummy HTML form and adding various fields to the form:
                                
	const formData = new FormData();
	formData.append('datachunk', encoded);
	formData.append('datalen', thisChunk);
	formData.append('chunksize', chunkSize);
	formData.append('filename', file.name);
	formData.append('index', index);

The form is then URL encoded before sending to the ESP32-P4 Nano:

	const encodedData = new URLSearchParams(formData).toString();

This puts the data into a format that allowed the POST data to be parsed using code I already had. The data format is similar to the following:

	datachunk=W29iamVjdCBCbG9iXQ%3D%3D&datalen=13&chunksize=512&filename=TEST.TXT&index=0

The datachunk field contains the Base64 data that needed to be decoded to produce the original binary data. However, this data format introduced a complication.

The Base64 encoding will pad data with the equals (=) symbol until the data is a multiple of 4 bytes. For example, a Base64 sequence of W29ia would need === to pad the sequence to 8 bytes e.g. W29ia===.

The URL encoding stage on the JavaScript side, converts any = symbol into %3D, as %3D cannot be Base64 decoded. So, any %3D sequence has to be decoded back to = before attempting any Base64 decode. This requires the following steps to Base64 decode the datachunk field:

1. Locate the datachunk field in the POST data.
2. Determine the length of the datachunk field.
3. Allocate a buffer to hold a new version of the datachunk field.
4. Parse the datachunk field, converting %3D to =, into the new buffer.
5. Use the new buffer to Base64 decode the data.
       
I was not thrilled with the extra memory usage, but I had to balance that against writing additional code to parse multipart form data.

## Memory Usage and Management

Restricting the subdirectory depth on file browse and download helps with memory usage. The application defaults to a depth of 4 subdirectories and allows a maximum of 8. At the default setting, subdirectory trees like /one/two/three/four/* can be listed. 

The number of files in a directory and the number subdirectories is unknown. The current mechanism uses a recursive function (ListDirectory) to discover content, sending file and subdirectory information as chunks of HTML data. The ListDirectory() function uses a combination of stack and heap buffers. Limiting the maximum subdirectory depth to 8 limits the amount of memory consumed by this feature.

I considered locating all files and subdirectories, adding the content information to a vector, and then iterating the vector to send the information to the browser. However, I was concerned that this might consume a considerable amount of memory. Every vector entry has an overhead, as does any structure required to keep track of the full file paths.

To avoid some ugly error handling code, and to reduce code size, many of the buffers that are heap allocated utilize the GCC cleanup attribute. If you are not familiar with this attribute, it allows a function to be called which passes a pointer to the heap allocated variable, when the variable goes out of scope. Here’s an example:

	// I get called when my assigned variable goes out of scope
	void DoSomeCleanup (char **buffer)
	{
		// Free the buffer
		free(*buffer);
	}
  
	bool SomeFunction (void)
	{
		// Call DoSomeCleanup(&myBuffer) when myBuffer goes out of scope 
		char *myBuffer __attribute__((cleanup(DoSomeCleanup))) = (char *) malloc(10);

		..	More Code	..

		return true;	// myBuffer goes out of scope and DoSomeCleanup(&myBuffer) is called
	}

## Limitations

The application has the following limitations:

1. Only the FAT file system is supported. There might be a way to get exFAT support, but NTFS would not be possible.
       
2. The ESP32-P4 seems to be sensitive to the type of USB drive used. Most of the name brand drives I tried worked, but I have a number of off-brand drives that could not be recognized.

## USB Sharing
It’s easy to move USB drive files around when connecting a smartphone or tablet to this device. A number of corporations have blocked USB access to their laptops and desktops. Using this device, it could be possible to move USB Drive files around. However, a number of corporations also block the 192.168.x.x subnet which means the device’s access point would have to moved to 10.x.x.x which is less likely to be blocked.

## Next Steps
The basic mechanism and concepts of this project seem to work well. I have the following thoughts on improving the project:

1. Moving files around on USB drives I trust shows the mechanisms work, but does not prove it would stop malware. I need to think about setting up an isolated environment and infecting a USB drive to see if this concept really works.
       
2. For prototyping, accessing the application from a browser using 192.168.4.1 is fine. However, it would be better if the end user had easier access to the web pages. I quickly tried mDNS to see if I could access the web pages using the hostname usbhost, but this did not work. The ESP-IDF examples have a Captive Portal example which I might try. Captive Portals give you that option, on smartphones, to click a link to access a signup or sign-on page. I think this would be better than having to type in an IP address.
       
3. I need to run some experiments with multiple files selected for upload and download. I restricted file upload and download to a single file so that I could verify the concept, but this is not very convenient or efficient.
   
4. I would like to add a sector explorer to the application. This might be a useful tool to inspect various sectors on the USB drive to see if any malicious content exists. 
