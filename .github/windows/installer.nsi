Unicode true
Name "Haruna SharePlay"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Haruna SharePlay"
RequestExecutionLevel user
SetCompressor zlib

Section
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  CreateDirectory "$SMPROGRAMS\Haruna SharePlay"
  CreateShortcut "$SMPROGRAMS\Haruna SharePlay\Haruna.lnk" "$INSTDIR\bin\haruna.exe"
SectionEnd

