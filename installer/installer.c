/* Spatial Ink installer. Copies only its bundled, checksum-verified CLAP file.
   MIT license, see ../LICENSE.txt. No downloads or registry changes. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define WINVER 0x0601
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

#define PAYLOAD_RESOURCE 101
#define PATH_CAP 1024
static const char expected_sha256[]="7e700452fae43ea64199e73747b2a29afe9b9282e7b21b5aca07164e07613cf4";

static bool verified(const uint8_t*bytes,DWORD length){
 if(!bytes||length<1024||bytes[0]!='M'||bytes[1]!='Z')return false;
 BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;PUCHAR object=NULL;
 ULONG object_size=0,result_size=0;uint8_t digest[32];bool ok=false;
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)goto done;
 if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&object_size,sizeof(object_size),&result_size,0)<0)goto done;
 object=calloc(1,object_size);if(!object)goto done;
 if(BCryptCreateHash(algorithm,&hash,object,object_size,NULL,0,0)<0)goto done;
 if(BCryptHashData(hash,(PUCHAR)bytes,length,0)<0||BCryptFinishHash(hash,digest,sizeof(digest),0)<0)goto done;
 ok=true;for(int i=0;i<32;i++){char encoded[3];snprintf(encoded,sizeof(encoded),"%02x",digest[i]);if(encoded[0]!=expected_sha256[i*2]||encoded[1]!=expected_sha256[i*2+1]){ok=false;break;}}
 done:if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);free(object);return ok;
}
static const uint8_t*payload(DWORD*size){
 HMODULE self=GetModuleHandleW(NULL);HRSRC resource=FindResourceW(self,MAKEINTRESOURCEW(PAYLOAD_RESOURCE),RT_RCDATA);
 if(!resource)return NULL;HGLOBAL data=LoadResource(self,resource);if(!data)return NULL;*size=SizeofResource(self,resource);return LockResource(data);
}
static bool filename(wchar_t*out,const wchar_t*folder,const wchar_t*name){
 int count=_snwprintf(out,PATH_CAP,L"%ls\\%ls",folder,name);return count>=0&&count<PATH_CAP;
}
/* Write a new temporary file in the destination directory, flush it, then
   atomically replace the plugin. A locked plugin leaves the old file intact. */
static DWORD install_to(const wchar_t*folder,const uint8_t*bytes,DWORD size){
 if(!verified(bytes,size))return ERROR_CRC;
 if(!CreateDirectoryW(folder,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return GetLastError();
 DWORD attributes=GetFileAttributesW(folder);if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))return ERROR_DIRECTORY;
 wchar_t final[PATH_CAP],temporary[PATH_CAP],temp_name[100];
 if(!filename(final,folder,L"SpatialInk.clap"))return ERROR_FILENAME_EXCED_RANGE;
 HANDLE file=INVALID_HANDLE_VALUE;
 for(int attempt=0;attempt<32;attempt++){
  _snwprintf(temp_name,100,L".SpatialInk.%lu.%d.installing",GetCurrentProcessId(),attempt);
  if(!filename(temporary,folder,temp_name))return ERROR_FILENAME_EXCED_RANGE;
  file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
  if(file!=INVALID_HANDLE_VALUE)break;
  if(GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)return GetLastError();
 }
 if(file==INVALID_HANDLE_VALUE)return ERROR_FILE_EXISTS;
 DWORD error=ERROR_SUCCESS,written=0;
 if(!WriteFile(file,bytes,size,&written,NULL))error=GetLastError();else if(written!=size)error=ERROR_WRITE_FAULT;
 if(!error&&!FlushFileBuffers(file))error=GetLastError();
 CloseHandle(file);
 if(!error&&!MoveFileExW(temporary,final,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))error=GetLastError();
 if(error)DeleteFileW(temporary);
 return error;
}

#ifndef INSTALLER_TEST
static void error_dialog(DWORD error){
 wchar_t system[500]={0},message[1600];FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,error,0,system,500,NULL);
 if(error==ERROR_SHARING_VIOLATION||error==ERROR_ACCESS_DENIED)
  _snwprintf(message,1600,L"تعذّر نسخ البلجن. اقفل FL Studio وأي برنامج فاتح Spatial Ink، ثم شغّل ملف التسطيب مرة ثانية.\n\nرمز الخطأ: %lu\n%ls",error,system);
 else _snwprintf(message,1600,L"لم يتم التثبيت.\n\nرمز الخطأ: %lu\n%ls",error,system);
 MessageBoxW(NULL,message,L"Spatial Ink — تعذّر التثبيت",MB_OK|MB_ICONERROR|MB_RTLREADING|MB_RIGHT);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command,int show){
 (void)instance;(void)previous;(void)command;(void)show;
 DWORD size=0;const uint8_t*bytes=payload(&size);if(!verified(bytes,size)){error_dialog(ERROR_CRC);return 1;}
 wchar_t common[MAX_PATH],folder[PATH_CAP],final[PATH_CAP];
 HRESULT result=SHGetFolderPathW(NULL,CSIDL_PROGRAM_FILES_COMMON,NULL,SHGFP_TYPE_CURRENT,common);
 if(FAILED(result)||!filename(folder,common,L"CLAP")||!filename(final,folder,L"SpatialInk.clap")){error_dialog(ERROR_PATH_NOT_FOUND);return 1;}
 wchar_t body[1800];_snwprintf(body,1800,L"سيتم نسخ بلجن Spatial Ink 0.1.0 للهيدفون إلى:\n\n%ls\n\nاقفل FL Studio قبل التثبيت أو تحديث البلجن.\nيتطلب FL Studio 2024.1 أو أحدث.\n\nالتسطيب يضيف ملف البلجن فقط، ويمكنك حذفه من نفس الفولدر لاحقًا.",final);
 INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
 const TASKDIALOG_BUTTON buttons[]={{100,L"تثبيت"}};
 TASKDIALOGCONFIG dialog={0};dialog.cbSize=sizeof(dialog);dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_RTL_LAYOUT|TDF_SIZE_TO_CONTENT;
 dialog.pszWindowTitle=L"Spatial Ink — تسطيب البلجن";dialog.pszMainInstruction=L"تثبيت Spatial Ink";dialog.pszContent=body;
 dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;dialog.pButtons=buttons;dialog.cButtons=1;dialog.nDefaultButton=100;dialog.pszMainIcon=TD_INFORMATION_ICON;
 int choice=0;result=TaskDialogIndirect(&dialog,&choice,NULL,NULL);
 if(FAILED(result)){choice=MessageBoxW(NULL,body,L"Spatial Ink — اضغط OK للتثبيت",MB_OKCANCEL|MB_ICONINFORMATION|MB_RTLREADING|MB_RIGHT)==IDOK?100:0;}
 if(choice!=100)return 0;
 DWORD error=install_to(folder,bytes,size);if(error){error_dialog(error);return 1;}
 _snwprintf(body,1800,L"تم تثبيت Spatial Ink بنجاح.\n\nافتح FL Studio ثم:\nOptions > File settings > Manage plugins\n\nفعّل Verify plugins وRescan previously verified plugins، ثم اضغط Find installed plugins.\n\nستلاقي Spatial Ink ضمن إفكتات الميكسر.\n\nمكان الملف:\n%ls",final);
 MessageBoxW(NULL,body,L"Spatial Ink — تم التثبيت",MB_OK|MB_ICONINFORMATION|MB_RTLREADING|MB_RIGHT);return 0;
}
#else
/* Test build omits the administrator manifest and tests only a scratch folder.
   The shipping installer has no destination override or unattended mode. */
#define CHECK(x,why) do{if(!(x)){fprintf(stderr,"FAIL: %s, line %d\n",why,__LINE__);return 1;}}while(0)
static bool matches(const wchar_t*path,const uint8_t*bytes,DWORD size){
 HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);if(f==INVALID_HANDLE_VALUE)return false;
 DWORD actual=GetFileSize(f,NULL),got=0;uint8_t*data=malloc(size);bool ok=data&&actual==size&&ReadFile(f,data,size,&got,NULL)&&got==size&&!memcmp(data,bytes,size);free(data);CloseHandle(f);return ok;
}
int main(int argc,char**argv){
 CHECK(argc==2,"scratch folder required");wchar_t folder[PATH_CAP],final[PATH_CAP];CHECK(MultiByteToWideChar(CP_UTF8,0,argv[1],-1,folder,PATH_CAP),"UTF8 scratch folder");CHECK(filename(final,folder,L"SpatialInk.clap"),"target filename");
 DWORD size=0;const uint8_t*bytes=payload(&size);CHECK(bytes&&verified(bytes,size),"bundled resource SHA256");
 CHECK(install_to(folder,bytes,size)==ERROR_SUCCESS,"first installation");CHECK(matches(final,bytes,size),"installed byte-identical plugin");
 CHECK(install_to(folder,bytes,size)==ERROR_SUCCESS,"repeated installation");CHECK(matches(final,bytes,size),"repeated install bytes");
 HANDLE lock=CreateFileW(final,GENERIC_READ,0,NULL,OPEN_EXISTING,0,NULL);CHECK(lock!=INVALID_HANDLE_VALUE,"lock installed plugin");
 DWORD blocked=install_to(folder,bytes,size);CHECK(blocked==ERROR_SHARING_VIOLATION||blocked==ERROR_ACCESS_DENIED,"locked plugin update rejected");CloseHandle(lock);CHECK(matches(final,bytes,size),"locked update preserves old plugin");
 uint8_t*corrupt=malloc(size);CHECK(corrupt,"corruption test buffer");memcpy(corrupt,bytes,size);corrupt[size/2]^=0x41;
 CHECK(!verified(corrupt,size)&&install_to(folder,corrupt,size)==ERROR_CRC,"corrupt payload rejected before writing");free(corrupt);CHECK(matches(final,bytes,size),"corruption leaves plugin intact");
 wchar_t wildcard[PATH_CAP];CHECK(filename(wildcard,folder,L"*.installing"),"temporary wildcard");WIN32_FIND_DATAW found;HANDLE scan=FindFirstFileW(wildcard,&found);CHECK(scan==INVALID_HANDLE_VALUE,"no abandoned temporary files");
 printf("PASS embedded checksum, first/repeated installation, locked-file preservation, corruption rejection, temporary-file cleanup\n");return 0;
}
#endif
