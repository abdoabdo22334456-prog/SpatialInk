/* UNI VST3 installer. Copies only its bundled, checksum-verified file.
   MIT license, see ../LICENSE.txt. No downloads or registry changes.
   The plugin payload must be self-contained; no separately installed CLAP is loaded. */
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
#define PLUGIN_FILENAME L"UNI.vst3"
#define BACKUP_FILENAME L"UNI.vst3.pre-update.bak"
#define LOOSE_PLUGIN_COLLISION ((DWORD)0xE0010001)
#define LEGACY_PLUGIN_COLLISION ((DWORD)0xE0010002)
static const char expected_sha256[]="c92dc50176eb397052095af6583b3373d179b43b86601f44d1cca2452cd35085";

static bool verified_hash(const uint8_t*bytes,DWORD length,const char*expected){
 if(!bytes||length<1024||bytes[0]!='M'||bytes[1]!='Z')return false;
 BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;PUCHAR object=NULL;
 ULONG object_size=0,result_size=0;uint8_t digest[32];bool ok=false;
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)goto done;
 if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,(PUCHAR)&object_size,sizeof(object_size),&result_size,0)<0)goto done;
 object=calloc(1,object_size);if(!object)goto done;
 if(BCryptCreateHash(algorithm,&hash,object,object_size,NULL,0,0)<0)goto done;
 if(BCryptHashData(hash,(PUCHAR)bytes,length,0)<0||BCryptFinishHash(hash,digest,sizeof(digest),0)<0)goto done;
 ok=true;for(int i=0;i<32;i++){char encoded[3];snprintf(encoded,sizeof(encoded),"%02x",digest[i]);if(encoded[0]!=expected[i*2]||encoded[1]!=expected[i*2+1]){ok=false;break;}}
 done:if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);free(object);return ok;
}
static bool verified(const uint8_t*bytes,DWORD length){return verified_hash(bytes,length,expected_sha256);}
static const uint8_t*payload(DWORD*size){
 HMODULE self=GetModuleHandleW(NULL);HRSRC resource=FindResourceW(self,MAKEINTRESOURCEW(PAYLOAD_RESOURCE),RT_RCDATA);
 if(!resource)return NULL;HGLOBAL data=LoadResource(self,resource);if(!data)return NULL;*size=SizeofResource(self,resource);return LockResource(data);
}
static bool filename(wchar_t*out,const wchar_t*folder,const wchar_t*name){
 int count=_snwprintf(out,PATH_CAP,L"%ls\\%ls",folder,name);return count>=0&&count<PATH_CAP;
}
static DWORD checked_directory(const wchar_t*path){
 DWORD attributes=GetFileAttributesW(path);
 if(attributes==INVALID_FILE_ATTRIBUTES){
  DWORD error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return error;
  if(!CreateDirectoryW(path,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS)return GetLastError();
  attributes=GetFileAttributesW(path);
 }
 if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return ERROR_DIRECTORY;
 return ERROR_SUCCESS;
}
/* Only fixed known directory components are created. No recursive deletion or
   directory replacement occurs, and reparse points are rejected at every level. */
static DWORD prepare_layout(const wchar_t*folder,wchar_t*architecture){
 DWORD error=checked_directory(folder);if(error)return error;
 wchar_t bundle[PATH_CAP],contents[PATH_CAP];
 if(!filename(bundle,folder,PLUGIN_FILENAME))return ERROR_FILENAME_EXCED_RANGE;
 DWORD bundle_attributes=GetFileAttributesW(bundle);
 if(bundle_attributes!=INVALID_FILE_ATTRIBUTES&&!(bundle_attributes&FILE_ATTRIBUTE_DIRECTORY))return LOOSE_PLUGIN_COLLISION;
 error=checked_directory(bundle);if(error)return error;
 if(!filename(contents,bundle,L"Contents"))return ERROR_FILENAME_EXCED_RANGE;
 error=checked_directory(contents);if(error)return error;
 if(!filename(architecture,contents,L"x86_64-win"))return ERROR_FILENAME_EXCED_RANGE;
 return checked_directory(architecture);
}
/* Write and flush a new temporary file before replacing the plugin. Preserve the
   first pre-existing plugin in a non-plugin backup; never overwrite that backup.
   A locked plugin leaves the existing file intact. */
static DWORD install_to(const wchar_t*folder,const uint8_t*bytes,DWORD size){
 if(!verified(bytes,size))return ERROR_CRC;
 /* UNI retains the original VST3 class ID. Refuse duplicate installations under
    the old name; preserve every old file and let the user archive it explicitly. */
 wchar_t legacy[PATH_CAP];if(!filename(legacy,folder,L"SpatialInkOrbit.vst3"))return ERROR_FILENAME_EXCED_RANGE;
 DWORD legacy_attributes=GetFileAttributesW(legacy);
 if(legacy_attributes!=INVALID_FILE_ATTRIBUTES)return LEGACY_PLUGIN_COLLISION;
 DWORD legacy_error=GetLastError();if(legacy_error!=ERROR_FILE_NOT_FOUND&&legacy_error!=ERROR_PATH_NOT_FOUND)return legacy_error;
 wchar_t architecture[PATH_CAP];DWORD layout_error=prepare_layout(folder,architecture);if(layout_error)return layout_error;
 wchar_t final[PATH_CAP],backup[PATH_CAP],temporary[PATH_CAP],temp_name[100];
 if(!filename(final,architecture,PLUGIN_FILENAME)||!filename(backup,architecture,BACKUP_FILENAME))return ERROR_FILENAME_EXCED_RANGE;
 DWORD existing=GetFileAttributesW(final);
 if(existing!=INVALID_FILE_ATTRIBUTES&&(existing&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return ERROR_DIRECTORY;
 if(existing==INVALID_FILE_ATTRIBUTES&&GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)return GetLastError();
 HANDLE file=INVALID_HANDLE_VALUE;
 for(int attempt=0;attempt<32;attempt++){
  _snwprintf(temp_name,100,L".UNI.%lu.%d.installing",GetCurrentProcessId(),attempt);
  if(!filename(temporary,architecture,temp_name))return ERROR_FILENAME_EXCED_RANGE;
  file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
  if(file!=INVALID_HANDLE_VALUE)break;
  if(GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)return GetLastError();
 }
 if(file==INVALID_HANDLE_VALUE)return ERROR_FILE_EXISTS;
 DWORD error=ERROR_SUCCESS,written=0;
 if(!WriteFile(file,bytes,size,&written,NULL))error=GetLastError();else if(written!=size)error=ERROR_WRITE_FAULT;
 if(!error&&!FlushFileBuffers(file))error=GetLastError();
 CloseHandle(file);
 if(!error&&existing!=INVALID_FILE_ATTRIBUTES&&!CopyFileW(final,backup,TRUE)){
  DWORD backup_error=GetLastError();
  if(backup_error!=ERROR_FILE_EXISTS&&backup_error!=ERROR_ALREADY_EXISTS)error=backup_error;
  else{
   DWORD backup_attributes=GetFileAttributesW(backup);
   if(backup_attributes==INVALID_FILE_ATTRIBUTES||(backup_attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))error=ERROR_DIRECTORY;
  }
 }
 if(!error&&!MoveFileExW(temporary,final,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))error=GetLastError();
 if(error)DeleteFileW(temporary);
 return error;
}

#ifndef INSTALLER_TEST
static void error_dialog(DWORD error){
 wchar_t system[500]={0},message[1600];FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,error,0,system,500,NULL);
 if(error==LOOSE_PLUGIN_COLLISION)
  _snwprintf(message,1600,L"يوجد ملف VST3 قديم باسم UNI.vst3 في فولدر VST3، بينما هذا الإصدار يحتاج فولدر Bundle بالاسم نفسه.\n\nلم يتم حذف الملف أو تغييره. انقله يدويًا لمكان احتياطي خارج مسارات فحص البلجنز، ثم شغّل التثبيت مرة ثانية.");
 else if(error==LEGACY_PLUGIN_COLLISION)
  _snwprintf(message,1600,L"يوجد إصدار بالاسم القديم SpatialInkOrbit.vst3 في فولدر VST3. يحتفظ UNI بهوية البلجن نفسها، فلا تترك النسختين في مسارات الفحص معًا.\n\nلم يتم حذف النسخة القديمة أو تغييرها. اقفل FL Studio وانقل SpatialInkOrbit.vst3 يدويًا إلى مكان احتياطي خارج مسارات VST3، ثم شغّل التثبيت مرة ثانية.");
 else if(error==ERROR_SHARING_VIOLATION||error==ERROR_ACCESS_DENIED)
  _snwprintf(message,1600,L"تعذّر نسخ البلجن. اقفل FL Studio وأي برنامج فاتح UNI، ثم شغّل ملف التسطيب مرة ثانية.\n\nرمز الخطأ: %lu\n%ls",error,system);
 else _snwprintf(message,1600,L"لم يتم التثبيت.\n\nرمز الخطأ: %lu\n%ls",error,system);
 MessageBoxW(NULL,message,L"UNI — تعذّر التثبيت",MB_OK|MB_ICONERROR|MB_RTLREADING|MB_RIGHT);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR command,int show){
 (void)instance;(void)previous;(void)command;(void)show;
 DWORD size=0;const uint8_t*bytes=payload(&size);if(!verified(bytes,size)){error_dialog(ERROR_CRC);return 1;}
 wchar_t common[MAX_PATH],folder[PATH_CAP],final[PATH_CAP];
 HRESULT result=SHGetFolderPathW(NULL,CSIDL_PROGRAM_FILES_COMMON,NULL,SHGFP_TYPE_CURRENT,common);
 if(FAILED(result)||!filename(folder,common,L"VST3")||!filename(final,folder,PLUGIN_FILENAME)){error_dialog(ERROR_PATH_NOT_FOUND);return 1;}
 wchar_t body[1800];_snwprintf(body,1800,L"سيتم تثبيت UNI 0.8.0 بصيغة VST3 Bundle إلى:\n\n%ls\n\nاقفل FL Studio وأي برنامج فاتح البلجن قبل التثبيت.\nلا يحتاج هذا الإصدار تثبيت CLAP منفصل.\n\nملف البلجن موجود داخل Contents\\x86_64-win. عند تحديثه تُحفظ أول نسخة احتياطية باسم UNI.vst3.pre-update.bak بجانبه.\nملفات CLAP وأي بلجنز أخرى لا تتغير.",final);
 INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
 const TASKDIALOG_BUTTON buttons[]={{100,L"تثبيت"}};
 TASKDIALOGCONFIG dialog={0};dialog.cbSize=sizeof(dialog);dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_RTL_LAYOUT|TDF_SIZE_TO_CONTENT;
 dialog.pszWindowTitle=L"UNI VST3 — تسطيب البلجن";dialog.pszMainInstruction=L"تثبيت UNI VST3";dialog.pszContent=body;
 dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;dialog.pButtons=buttons;dialog.cButtons=1;dialog.nDefaultButton=100;dialog.pszMainIcon=TD_INFORMATION_ICON;
 int choice=0;result=TaskDialogIndirect(&dialog,&choice,NULL,NULL);
 if(FAILED(result)){choice=MessageBoxW(NULL,body,L"UNI — اضغط OK للتثبيت",MB_OKCANCEL|MB_ICONINFORMATION|MB_RTLREADING|MB_RIGHT)==IDOK?100:0;}
 if(choice!=100)return 0;
 DWORD error=install_to(folder,bytes,size);if(error){error_dialog(error);return 1;}
 _snwprintf(body,1800,L"تم تثبيت UNI بصيغة VST3 بنجاح.\n\nافتح FL Studio ثم:\nOptions > File settings > Manage plugins\n\nفعّل Verify plugins وRescan previously verified plugins، ثم اضغط Find installed plugins.\n\nاختَر نسخة VST3 من UNI ضمن إفكتات الميكسر.\n\nمكان الملف:\n%ls",final);
 MessageBoxW(NULL,body,L"UNI — تم التثبيت",MB_OK|MB_ICONINFORMATION|MB_RTLREADING|MB_RIGHT);return 0;
}
#else
/* Test build omits the administrator manifest and tests only a scratch folder.
   The shipping installer has no destination override or unattended mode. */
#define CHECK(x,why) do{if(!(x)){fprintf(stderr,"FAIL: %s, line %d\n",why,__LINE__);return 1;}}while(0)
static bool matches(const wchar_t*path,const uint8_t*bytes,DWORD size){
 HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);if(f==INVALID_HANDLE_VALUE)return false;
 DWORD actual=GetFileSize(f,NULL),got=0;uint8_t*data=malloc(size);bool ok=data&&actual==size&&ReadFile(f,data,size,&got,NULL)&&got==size&&!memcmp(data,bytes,size);free(data);CloseHandle(f);return ok;
}
static bool write_fixture(const wchar_t*path,const uint8_t*bytes,DWORD size){
 HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);if(f==INVALID_HANDLE_VALUE)return false;
 DWORD written=0;bool ok=WriteFile(f,bytes,size,&written,NULL)&&written==size;CloseHandle(f);return ok;
}
int main(int argc,char**argv){
 CHECK(argc==2,"scratch folder required");wchar_t root[PATH_CAP],folder[PATH_CAP],final[PATH_CAP],case_name[100];CHECK(MultiByteToWideChar(CP_UTF8,0,argv[1],-1,root,PATH_CAP),"UTF8 scratch folder");
 CHECK(CreateDirectoryW(root,NULL)||GetLastError()==ERROR_ALREADY_EXISTS,"scratch parent directory");
 _snwprintf(case_name,100,L"case-%lu-%llu",GetCurrentProcessId(),(unsigned long long)GetTickCount64());CHECK(filename(folder,root,case_name),"unique scratch case");CHECK(CreateDirectoryW(folder,NULL),"fresh scratch case");
 CHECK(filename(final,folder,L"UNI.vst3\\Contents\\x86_64-win\\UNI.vst3"),"target bundle filename");
 DWORD size=0;const uint8_t*bytes=payload(&size);CHECK(bytes&&verified(bytes,size),"bundled resource SHA256");
 CHECK(install_to(folder,bytes,size)==ERROR_SUCCESS,"first installation");CHECK(matches(final,bytes,size),"installed byte-identical plugin");
 CHECK(install_to(folder,bytes,size)==ERROR_SUCCESS,"repeated installation");CHECK(matches(final,bytes,size),"repeated install bytes");
 HANDLE lock=CreateFileW(final,GENERIC_READ,0,NULL,OPEN_EXISTING,0,NULL);CHECK(lock!=INVALID_HANDLE_VALUE,"lock installed plugin");
 DWORD blocked=install_to(folder,bytes,size);CHECK(blocked==ERROR_SHARING_VIOLATION||blocked==ERROR_ACCESS_DENIED,"locked plugin update rejected");CloseHandle(lock);CHECK(matches(final,bytes,size),"locked update preserves old plugin");
 uint8_t*corrupt=malloc(size);CHECK(corrupt,"corruption test buffer");memcpy(corrupt,bytes,size);corrupt[size/2]^=0x41;
 CHECK(!verified(corrupt,size)&&install_to(folder,corrupt,size)==ERROR_CRC,"corrupt payload rejected before writing");free(corrupt);CHECK(matches(final,bytes,size),"corruption leaves plugin intact");
 wchar_t wildcard[PATH_CAP],architecture[PATH_CAP];CHECK(prepare_layout(folder,architecture)==ERROR_SUCCESS&&filename(wildcard,architecture,L"*.installing"),"temporary wildcard");WIN32_FIND_DATAW found;HANDLE scan=FindFirstFileW(wildcard,&found);CHECK(scan==INVALID_HANDLE_VALUE,"no abandoned temporary files");
 wchar_t upgrade[PATH_CAP],old_plugin[PATH_CAP],backup[PATH_CAP],unrelated[PATH_CAP];
 CHECK(filename(upgrade,folder,L"upgrade")&&CreateDirectoryW(upgrade,NULL),"upgrade scratch directory");
 CHECK(prepare_layout(upgrade,architecture)==ERROR_SUCCESS&&filename(old_plugin,architecture,PLUGIN_FILENAME)&&filename(backup,architecture,BACKUP_FILENAME)&&filename(unrelated,upgrade,L"UNI.clap"),"upgrade fixture names");
 const uint8_t legacy_a[]="original VST3 plugin fixture A",legacy_b[]="later VST3 plugin fixture B",clap_fixture[]="unrelated CLAP remains intact";
 CHECK(write_fixture(old_plugin,legacy_a,sizeof(legacy_a))&&write_fixture(unrelated,clap_fixture,sizeof(clap_fixture)),"write old plugin and unrelated CLAP fixtures");
 CHECK(install_to(upgrade,bytes,size)==ERROR_SUCCESS&&matches(old_plugin,bytes,size),"replace pre-existing VST3");
 CHECK(matches(backup,legacy_a,sizeof(legacy_a)),"preserve original pre-update backup");
 CHECK(write_fixture(old_plugin,legacy_b,sizeof(legacy_b)),"simulate later pre-existing plugin");
 CHECK(install_to(upgrade,bytes,size)==ERROR_SUCCESS&&matches(backup,legacy_a,sizeof(legacy_a)),"never overwrite first backup on later upgrades");
 CHECK(matches(unrelated,clap_fixture,sizeof(clap_fixture)),"CLAP file untouched");
 wchar_t directory_case[PATH_CAP],directory_target[PATH_CAP];CHECK(filename(directory_case,folder,L"directory-case")&&prepare_layout(directory_case,architecture)==ERROR_SUCCESS&&filename(directory_target,architecture,PLUGIN_FILENAME)&&CreateDirectoryW(directory_target,NULL),"directory collision at inner DLL fixture");
 CHECK(install_to(directory_case,bytes,size)==ERROR_DIRECTORY,"reject directory at inner DLL without recursive replacement");
 wchar_t loose_case[PATH_CAP],loose_target[PATH_CAP];CHECK(filename(loose_case,folder,L"loose-case")&&CreateDirectoryW(loose_case,NULL)&&filename(loose_target,loose_case,PLUGIN_FILENAME)&&write_fixture(loose_target,legacy_a,sizeof(legacy_a)),"loose VST3 collision fixture");
 CHECK(install_to(loose_case,bytes,size)==LOOSE_PLUGIN_COLLISION&&matches(loose_target,legacy_a,sizeof(legacy_a)),"reject and preserve loose file at bundle root");
 wchar_t legacy_file_case[PATH_CAP],legacy_file_target[PATH_CAP];
 CHECK(filename(legacy_file_case,folder,L"legacy-file-case")&&CreateDirectoryW(legacy_file_case,NULL)&&filename(legacy_file_target,legacy_file_case,L"SpatialInkOrbit.vst3")&&write_fixture(legacy_file_target,legacy_a,sizeof(legacy_a)),"old-name file fixture");
 CHECK(install_to(legacy_file_case,bytes,size)==LEGACY_PLUGIN_COLLISION&&matches(legacy_file_target,legacy_a,sizeof(legacy_a)),"reject and preserve old-name file to avoid duplicate class ID");
 wchar_t legacy_dir_case[PATH_CAP],legacy_dir_target[PATH_CAP],legacy_dir_file[PATH_CAP];
 CHECK(filename(legacy_dir_case,folder,L"legacy-directory-case")&&CreateDirectoryW(legacy_dir_case,NULL)&&filename(legacy_dir_target,legacy_dir_case,L"SpatialInkOrbit.vst3")&&CreateDirectoryW(legacy_dir_target,NULL)&&filename(legacy_dir_file,legacy_dir_target,L"preserve.txt")&&write_fixture(legacy_dir_file,legacy_b,sizeof(legacy_b)),"old-name bundle fixture");
 CHECK(install_to(legacy_dir_case,bytes,size)==LEGACY_PLUGIN_COLLISION&&matches(legacy_dir_file,legacy_b,sizeof(legacy_b)),"reject and preserve old-name bundle without moving or deleting it");
 printf("PASS bundled checksum, fixed bundle layout, first/repeated installation, locked-file preservation, corruption rejection, temporary-file cleanup, original backup preservation, unrelated CLAP preservation, inner-directory rejection, loose-root preservation, old-name file/bundle collision preservation\n");return 0;
}
#endif
