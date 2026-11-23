param(
    [Parameter(Mandatory=$true)][ValidateSet('Inspect','Protect','Restore')][string]$Operation,
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$AllowedParent,
    [Parameter(Mandatory=$true)][string]$Evidence,
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$ResourceRoot
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$packagePath = (Resolve-Path -LiteralPath $Package).ProviderPath.TrimEnd('\')
$parentPath = (Resolve-Path -LiteralPath $AllowedParent).ProviderPath.TrimEnd('\')
$evidencePath = (Resolve-Path -LiteralPath $Evidence).ProviderPath.TrimEnd('\')
$projectPath = (Resolve-Path -LiteralPath $ProjectRoot).ProviderPath.TrimEnd('\')
$resourcePath = (Resolve-Path -LiteralPath $ResourceRoot).ProviderPath.TrimEnd('\')
function Within([string]$Child, [string]$Root) {
    return $Child.Equals($Root, [StringComparison]::OrdinalIgnoreCase) -or
        $Child.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase)
}
if (-not (Within $packagePath $parentPath) -or $packagePath -eq $parentPath) {
    throw 'Package must be strictly inside its explicitly allowed parent'
}
foreach ($protectedPath in @($projectPath, $resourcePath)) {
    if ((Within $packagePath $protectedPath) -or (Within $protectedPath $packagePath)) {
        throw 'Package ACL must not affect the workspace or original resources'
    }
}
if ((Within $evidencePath $packagePath) -or (Within $packagePath $evidencePath)) {
    throw 'ACL evidence and backup must be disjoint from package'
}
$cursorPath = $packagePath
while ($true) {
    if ((Get-Item -LiteralPath $cursorPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Package ancestry must not contain reparse points'
    }
    $nextPath = (Get-Item -LiteralPath $cursorPath -Force).Parent.FullName
    if (-not $nextPath -or $nextPath -eq $cursorPath) { break }
    $cursorPath = $nextPath
}
if ($Operation -eq 'Protect') {
    foreach ($required in @('main_game.exe', 'game-export-manifest.json', 'config\config.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $packagePath $required) -PathType Leaf)) {
            throw "Package input missing: $required"
        }
    }
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$sid = $identity.User
$sections = [Security.AccessControl.AccessControlSections]::Access -bor
    [Security.AccessControl.AccessControlSections]::Owner -bor
    [Security.AccessControl.AccessControlSections]::Group
$backupPath = Join-Path $evidencePath 'package-acl-backup.json'
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RvtPackageAclAccess {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("advapi32.dll", SetLastError=true)]
    private static extern bool SetKernelObjectSecurity(SafeFileHandle handle, uint information, byte[] descriptor);
    public static void RequireRestoreRights(string path) {
        using (var handle = CreateFile(path, 0x00060000, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Package root requires READ_CONTROL and WRITE_DAC before any protection");
        }
    }
    public static void RestoreDacl(string path, byte[] descriptor) {
        using (var handle = CreateFile(path, 0x00060000, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (!SetKernelObjectSecurity(handle, 4, descriptor))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }
    }
}
'@
[RvtPackageAclAccess]::RequireRestoreRights($packagePath)
$acl = Get-Acl -LiteralPath $packagePath
if ($Operation -eq 'Inspect') {
    @{status='inspected-no-mutation';package=$packagePath;sid=$sid.Value;
      ownerSid=$acl.GetOwner([Security.Principal.SecurityIdentifier]).Value;
      sddl=$acl.GetSecurityDescriptorSddlForm($sections);
      readControlAndWriteDacOpenSucceeded=$true;ownerMutationRequired=$false;
      scope='Existing directory handle access check only; no ACL/owner/file writes in package'} |
        ConvertTo-Json -Compress
    exit 0
}
if ($Operation -eq 'Restore') {
    $backup = Get-Content -LiteralPath $backupPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($backup.package -ne $packagePath -or $backup.sid -ne $sid.Value) {
        throw 'ACL backup package/SID identity differs'
    }
    $restored = Get-Acl -LiteralPath $packagePath
    $restored.SetSecurityDescriptorSddlForm(
        $backup.sddl, [Security.AccessControl.AccessControlSections]::Access)
    Set-Acl -LiteralPath $packagePath -AclObject $restored
    $actual = (Get-Acl -LiteralPath $packagePath).GetSecurityDescriptorSddlForm($sections)
    if ($actual -ne $backup.sddl) {
        $raw = [Security.AccessControl.RawSecurityDescriptor]::new($backup.sddl)
        $bytes = [byte[]]::new($raw.BinaryLength)
        $raw.GetBinaryForm($bytes, 0)
        [RvtPackageAclAccess]::RestoreDacl($packagePath, $bytes)
        $actual = (Get-Acl -LiteralPath $packagePath).GetSecurityDescriptorSddlForm($sections)
    }
    if ($actual -ne $backup.sddl) { throw 'Restored ACL differs from original backup' }
    @{status='restored';package=$packagePath;sid=$sid.Value;exactSddlRestored=$true} |
        ConvertTo-Json -Compress
    exit 0
}
if (Test-Path -LiteralPath $backupPath) { throw 'Preserve existing ACL backup; use fresh evidence' }
$sddl = $acl.GetSecurityDescriptorSddlForm($sections)
@{package=$packagePath;sid=$sid.Value;sddl=$sddl;owner=$acl.Owner;
  ownerSid=$acl.GetOwner([Security.Principal.SecurityIdentifier]).Value;
  actor=$identity.Name;restoreRightsChecked=$true;ownerMutated=$false;
  allowedParent=$parentPath;scope='Exact new package root DACL only, inherited WRITE deny'} |
    ConvertTo-Json | Set-Content -LiteralPath $backupPath -Encoding UTF8
$rule = [Security.AccessControl.FileSystemAccessRule]::new(
    $sid, [Security.AccessControl.FileSystemRights]::Write,
    ([Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
     [Security.AccessControl.InheritanceFlags]::ObjectInherit),
    [Security.AccessControl.PropagationFlags]::None,
    [Security.AccessControl.AccessControlType]::Deny)
$acl.AddAccessRule($rule)
Set-Acl -LiteralPath $packagePath -AclObject $acl
$newProbe = Join-Path $packagePath ('.rvt-write-probe-' + [Guid]::NewGuid().ToString('N'))
$createDenied = $false
$existingWriteDenied = $false
try {
    try {
        $stream = [IO.File]::Open($newProbe, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        $stream.Dispose()
    } catch [UnauthorizedAccessException] { $createDenied = $true }
    try {
        $stream = [IO.File]::Open((Join-Path $packagePath 'config\config.json'),
            [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
        $stream.Dispose()
    } catch [UnauthorizedAccessException] { $existingWriteDenied = $true }
} finally {
    if (Test-Path -LiteralPath $newProbe) { Remove-Item -LiteralPath $newProbe -Force }
}
$probeExit = 0
if ($createDenied -and $existingWriteDenied) { $probeExit = 1 }
$result = @{status='protected';package=$packagePath;sid=$sid.Value;writeProbeExit=$probeExit;
    createFileDenied=$createDenied;existingFileWriteDenied=$existingWriteDenied;
    protection='NTFS inherited WRITE deny, not ReadOnly attributes';
    probeMethod='Actual FileStream CreateNew and existing-file Write open under current SID';
    outputScope='Screenshots and evidence writes outside install are explicit user-output'}
$result | ConvertTo-Json -Compress
if (-not $createDenied -or -not $existingWriteDenied) { throw 'Actual package write probes did not both fail' }
