# windows/package/ShortcutTools.ps1 - dot-sourced by install.ps1, uninstall.ps1 and test-package.ps1.
# Writes and reads a .lnk WITH its AppUserModelID (System.AppUserModel.ID), which WScript.Shell cannot set.
# The id and the shortcut's file name are the ones windows/src/notify.hpp declares (kAppId, kShortcutName): a
# toast from an unpackaged program is only shown when a Start-menu shortcut carries the sender's id.
# The exe has no switch that writes its shortcut for a given folder (PACKAGING.md asks for `--register`); until
# it has, this does what notify.hpp ensureShortcut does: IShellLink + IPropertyStore + IPersistFile.
# Nothing here knows a destination: every path is an argument.

. $(if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'edition.ps1')) { Join-Path $PSScriptRoot 'edition.ps1' } else { Join-Path $PSScriptRoot '..\edition.ps1' })   # the edition's names (beside it in a package; windows\edition.ps1 in a checkout)
# the edition's (core/deck/edition.hpp; in this copy: demo.claude-deck-native, "Claude Deck Native.lnk", ClaudeDeckNative.exe)
$script:DeckAppId = $DeckEdition.AppId                        # notify.hpp kAppId
$script:DeckShortcutName = $DeckEdition.Shortcut + '.lnk'     # notify.hpp kShortcutName
$script:DeckExeName = $DeckEdition.Exe
$script:DeckDescription = $DeckEdition.Description + ': all Claude Code sessions in one window'
$script:DeckProgramsDir = $DeckEdition.ProgramsDir            # %LOCALAPPDATA%\Programs\<this>
$script:DeckStateDir = $DeckEdition.StateDir                  # %APPDATA%\<this>

if (-not ('DeckPkg.Shortcut' -as [type])) {
  Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
namespace DeckPkg {
  [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
  class CShellLink { }

  [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
  interface IShellLinkW {
    void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder file, int cch, IntPtr findData, uint flags);
    void GetIDList(out IntPtr pidl);
    void SetIDList(IntPtr pidl);
    void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder name, int cch);
    void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string name);
    void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder dir, int cch);
    void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string dir);
    void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder args, int cch);
    void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string args);
    void GetHotkey(out short hotkey);
    void SetHotkey(short hotkey);
    void GetShowCmd(out int show);
    void SetShowCmd(int show);
    void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int cch, out int index);
    void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int index);
    void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
    void Resolve(IntPtr hwnd, uint flags);
    void SetPath([MarshalAs(UnmanagedType.LPWStr)] string file);
  }

  [StructLayout(LayoutKind.Sequential, Pack = 4)]
  struct PropertyKey { public Guid fmtid; public uint pid; }

  // a PROPVARIANT, as far as a VT_LPWSTR needs it (24 bytes on x64, 16 on x86)
  [StructLayout(LayoutKind.Sequential)]
  struct PropVariant { public ushort vt; public ushort r1; public ushort r2; public ushort r3; public IntPtr p; public IntPtr p2; }

  [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
  interface IPropertyStore {
    void GetCount(out uint count);
    void GetAt(uint index, out PropertyKey key);
    void GetValue(ref PropertyKey key, out PropVariant value);
    void SetValue(ref PropertyKey key, ref PropVariant value);
    void Commit();
  }

  public static class Shortcut {
    [DllImport("ole32.dll")] static extern int PropVariantClear(ref PropVariant pv);
    static PropertyKey AppIdKey() {
      PropertyKey k = new PropertyKey(); k.fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"); k.pid = 5; return k;
    }
    // Writes (replaces) the .lnk. The target is started directly: no interpreter in between.
    public static void Write(string lnk, string target, string arguments, string workDir, string description, string appId) {
      object o = new CShellLink();
      try {
        IShellLinkW sl = (IShellLinkW)o;
        sl.SetPath(target);
        sl.SetArguments(arguments ?? "");
        sl.SetWorkingDirectory(workDir ?? "");
        sl.SetIconLocation(target, 0);
        sl.SetDescription(description ?? "");
        if (!String.IsNullOrEmpty(appId)) {
          IPropertyStore ps = (IPropertyStore)o;
          PropertyKey k = AppIdKey();
          PropVariant pv = new PropVariant(); pv.vt = 31; pv.p = Marshal.StringToCoTaskMemUni(appId);   // VT_LPWSTR
          try { ps.SetValue(ref k, ref pv); ps.Commit(); } finally { PropVariantClear(ref pv); }
        }
        ((IPersistFile)o).Save(lnk, true);
      } finally { Marshal.FinalReleaseComObject(o); }
    }
    // { target, arguments, working directory, AppUserModelID } of a .lnk (read only)
    public static string[] Read(string lnk) {
      object o = new CShellLink();
      try {
        ((IPersistFile)o).Load(lnk, 0);
        IShellLinkW sl = (IShellLinkW)o;
        StringBuilder path = new StringBuilder(32768), args = new StringBuilder(32768), dir = new StringBuilder(32768);
        sl.GetPath(path, path.Capacity, IntPtr.Zero, 4);          // SLGP_RAWPATH
        sl.GetArguments(args, args.Capacity);
        sl.GetWorkingDirectory(dir, dir.Capacity);
        string id = "";
        IPropertyStore ps = (IPropertyStore)o;
        PropertyKey k = AppIdKey(); PropVariant pv;
        ps.GetValue(ref k, out pv);
        try { if (pv.vt == 31 && pv.p != IntPtr.Zero) id = Marshal.PtrToStringUni(pv.p); } finally { PropVariantClear(ref pv); }
        return new string[] { path.ToString(), args.ToString(), dir.ToString(), id };
      } finally { Marshal.FinalReleaseComObject(o); }
    }
  }
}
'@
}

function Write-DeckShortcut([string]$Lnk, [string]$Target, [string]$Arguments) {
  $dir = Split-Path -Parent $Lnk
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  [DeckPkg.Shortcut]::Write($Lnk, $Target, $Arguments, (Split-Path -Parent $Target), $script:DeckDescription, $script:DeckAppId)
}

function Read-DeckShortcut([string]$Lnk) {
  $r = [DeckPkg.Shortcut]::Read($Lnk)
  return [pscustomobject]@{ Target = $r[0]; Arguments = $r[1]; WorkingDirectory = $r[2]; AppId = $r[3] }
}

function Test-SamePath([string]$a, [string]$b) {
  if (-not $a -or -not $b) { return $false }
  return [string]::Equals([IO.Path]::GetFullPath($a).TrimEnd('\'), [IO.Path]::GetFullPath($b).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
}

# True when the file is held open by a running process (the deck is running from it).
function Test-FileLocked([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return $false }
  try { $s = [IO.File]::Open($Path, 'Open', 'ReadWrite', 'None'); $s.Close(); return $false } catch { return $true }
}
