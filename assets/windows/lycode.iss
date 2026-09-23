; LyCode — Windows 安装包脚本（Inno Setup 6）
;
; 本地编译（在仓库根目录）：
;   iscc /DMyAppVersion=0.1.0 /DMyArch=x64 /DMyArchAllowed=x64compatible ^
;        /DMySourceDir=%CD%\build\lycode /DMyOutputDir=%CD%\dist assets\windows\lycode.iss
;
; CI 的调用方式见 .github/workflows/release.yaml 的「编译 Windows 安装包」步骤。
;
; 说明：源目录（MySourceDir）指向 CMake 的安装暂存树，里面应当已经含
; windeployqt 部署好的 Qt DLL 与插件（bin\platforms、bin\sqldrivers…）。

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

; MyArch 只用于产物文件名；MyArchAllowed 是 Inno 的目标架构约束。
; 两者分开是因为文件名想写 x64，而 Inno 的约束值要写 x64compatible。
; 若要出 arm64 包，改成：
;   /DMyArch=arm64 /DMyArchAllowed=arm64
#ifndef MyArch
  #define MyArch "x64"
#endif
#ifndef MyArchAllowed
  #define MyArchAllowed "x64compatible"
#endif

#ifndef MySourceDir
  #define MySourceDir "..\..\build\lycode"
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\..\dist"
#endif
; 仓库根目录，用来取 LICENSE 等仓库内文件（相对本脚本所在目录）。
#ifndef MyRepoRoot
  #define MyRepoRoot "..\.."
#endif

#define MyAppName "LyCode"
#define MyAppExeName "lycode.exe"
#define MyAppPublisher "lhdjply"
#define MyAppUrl "https://github.com/lhdjply/LyCode"

[Setup]
; AppId 一旦发布就不要再改：升级安装与卸载识别都靠它。
AppId={{7C9F1E42-3B8A-4D16-9F27-5A6C8E0B1D34}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}
AppUpdatesURL={#MyAppUrl}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
; 默认按用户安装（不弹 UAC），需要时可在向导里切换为全机器安装。
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed={#MyArchAllowed}
ArchitecturesInstallIn64BitMode={#MyArchAllowed}
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-win-{#MyArch}-setup
SetupIconFile={#MyRepoRoot}\assets\icons\windows\lycode.ico
UninstallDisplayIcon={app}\bin\{#MyAppExeName}
LicenseFile={#MyRepoRoot}\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 6.3+ 才有 x64compatible / arm64 这两个约束值。
MinVersion=10.0

[Languages]
; 只有英文向导。要中文向导需另加非官方翻译文件（Inno Setup 不自带
; ChineseSimplified.isl），放到 assets/windows/ 后再加一行：
;   Name: "chinese"; MessagesFile: "compiler:Default.isl,{#MyRepoRoot}\assets\windows\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; bin 下是主程序 + windeployqt 部署的 Qt DLL 与插件（platforms/sqldrivers/…），必须递归。
Source: "{#MySourceDir}\bin\*"; DestDir: "{app}\bin"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#MySourceDir}\share\*"; DestDir: "{app}\share"; Flags: recursesubdirs createallsubdirs ignoreversion skipifsourcedoesntexist
Source: "{#MyRepoRoot}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
