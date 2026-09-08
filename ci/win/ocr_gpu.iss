#ifndef BUILD_NUMBER
#define BUILD_NUMBER "0"
#endif
[Setup]
AppId={{43B53F7F-87AC-4667-B837-6D32745FEC18}
AppName=YACReader NVIDIA OCR Add-on
AppVersion=1.{#BUILD_NUMBER}
DefaultDirName={autopf}\YACReader
DisableDirPage=no
DisableProgramGroupPage=yes
UninstallFilesDir={app}\ocr-neural-gpu\uninstall
OutputDir=OutputGpu
OutputBaseFilename=YACReader-NVIDIA-OCR-{#BUILD_NUMBER}-win64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/fast
SolidCompression=yes
PrivilegesRequired=admin
[Files]
Source: "gpu_contents\*"; DestDir: "{app}\ocr-neural-gpu"; Flags: ignoreversion recursesubdirs createallsubdirs
[Code]
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not FileExists(ExpandConstant('{app}\ocr-neural\worker.py')) then
  begin
    MsgBox('Choose the folder containing the updated YACReader installation first.', mbError, MB_OK);
    Result := False;
  end;
end;
