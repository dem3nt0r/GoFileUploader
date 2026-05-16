# GoFile WinHTTP Uploader

A Windows CLI tool that uploads files to GoFile using the WinHTTP API.  
The upload is implemented using **manual multipart/form-data construction** and **streaming file I/O (no full memory buffering)**.

---

## 🚀 Usage

```bash
uploader.exe <file_path>
```

### 📤 Output:
#### Success
```bash
Upload Success. Download link -> https://gofile.io/d/xxxxxx
```
#### Failure
```bash
Upload failed: <error message>
```

## 📤 Output
### Success

## ⚙️ How it works
- Open file and get size  
- Create WinHTTP request (multipart/form-data)  
- Stream file in 64KB chunks  
- Send request and receive response  
- Extract download URL from response

## 📡 API Reference
Upload endpoint:
```
https://upload.gofile.io/uploadfile
```
Official documentation:
https://gofile.io/api

## 📌 Limitations
* No retry / reconnect mechanism
* No upload progress indicator
* No async or parallel upload