<#
    What a host sees of a built Windows DLL, with no GL and no Resolume.

        tools/wincheck.ps1 -Dll build/Release/Hyacinth.dll -Name "SW Hyacinth" -Id HY01 `
                           -Kind effect -Params 47

    It LOADS the DLL (so a missing runtime or dependency fails here, where
    dumpbin cannot see it), finds plugMain, and asks it what Resolume asks
    first: FF_GET_INFO (the name, read back as the char[16] a host reads, the
    id and the type) and the parameters (their count and names) -- which the
    SDK answers from a prototype instance, so the constructor runs too.

    Slitscan's (isohypse's, without its worlds): hyacinth embeds no data. release.yml runs
    it on the DLL it builds. The pixels are checked
    on a Mac (tools/verify.sh); a Windows runner has no GL to draw them with.
#>
param(
    [Parameter( Mandatory )] [string] $Dll,
    [Parameter( Mandatory )] [string] $Name,
    [Parameter( Mandatory )] [string] $Id,
    [Parameter( Mandatory )] [ValidateSet( 'source', 'effect' )] [string] $Kind,
    [Parameter( Mandatory )] [int] $Params
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Ffgl
{
    [DllImport( "kernel32", SetLastError = true, CharSet = CharSet.Unicode )]
    public static extern IntPtr LoadLibraryW( string path );
    [DllImport( "kernel32", SetLastError = true, CharSet = CharSet.Ansi )]
    public static extern IntPtr GetProcAddress( IntPtr module, string name );
    // FFMixed is a union of a 32-bit integer and a pointer: pointer-sized.
    [UnmanagedFunctionPointer( CallingConvention.StdCall )]
    public delegate IntPtr PlugMain( uint code, IntPtr input, IntPtr instance );
}
'@

$failures = 0
function Check( [bool] $ok, [string] $what )
{
    if( $ok ) { Write-Host "  ok    $what" }
    else { Write-Host "  FAIL  $what"; $script:failures++ }
}

$path = ( Resolve-Path $Dll ).Path
Write-Host "$path"
$module = [Ffgl]::LoadLibraryW( $path )
if( $module -eq [IntPtr]::Zero )
{
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw "LoadLibrary failed (Win32 error $err): a missing dependency or runtime"
}
$proc = [Ffgl]::GetProcAddress( $module, 'plugMain' )
Check ( $proc -ne [IntPtr]::Zero ) 'loads, and exports plugMain'
if( $proc -eq [IntPtr]::Zero ) { exit 1 }
$plugMain = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer( $proc, [Ffgl+PlugMain] )

# FF_GET_INFO (0): PluginInfoStruct -- two FFUInt32 API versions, char[4]
# id, char[16] name (neither null-terminated), FFUInt32 type.
$info  = $plugMain.Invoke( 0, [IntPtr]::Zero, [IntPtr]::Zero )
$bytes = New-Object byte[] 32
[Runtime.InteropServices.Marshal]::Copy( $info, $bytes, 0, 32 )
$gotId   = [Text.Encoding]::ASCII.GetString( $bytes, 8, 4 )
$gotName = [Text.Encoding]::ASCII.GetString( $bytes, 12, 16 ).TrimEnd( [char]0 )
$gotType = [BitConverter]::ToUInt32( $bytes, 28 )
$wantType = if( $Kind -eq 'source' ) { 1 } else { 0 }
Check ( $gotId -eq $Id ) "id $gotId (expected $Id)"
Check ( $gotName -eq $Name ) "name '$gotName', as a host reads char[16] (expected '$Name')"
Check ( $gotType -eq $wantType ) "type $gotType, $Kind"

# FF_GET_NUM_PARAMETERS (4) and FF_GET_PARAMETER_NAME (5): the SDK builds a
# prototype instance to answer, so the plugin's constructor runs here. The
# count is the low 32 bits of an FFMixed, as a host reads it: the high half of
# the register is whatever the plugin left there (333 on windows-latest's
# MSVC). 0xffffffffL, not 0xffffffff, which PowerShell reads as an Int32 -1
# and so masks nothing.
$count = [int]( $plugMain.Invoke( 4, [IntPtr]::Zero, [IntPtr]::Zero ).ToInt64() -band 0xffffffffL )
$names = @()
for( $i = 0; $i -lt $count; $i++ )
{
    $p = $plugMain.Invoke( 5, [IntPtr]$i, [IntPtr]::Zero )
    $names += [Runtime.InteropServices.Marshal]::PtrToStringAnsi( $p )
}
Check ( $count -eq $Params ) "$count parameters (expected $Params): $( $names[0] ) .. $( $names[-1] )"
Check ( $names[0] -eq 'Foil Area' -and $names -contains 'Groove Depth' -and $names -contains 'About' ) 'Foil Area first; Groove Depth and About among them'

if( $failures -gt 0 ) { Write-Host "`n  FAIL ($failures)"; exit 1 }
Write-Host "`n  PASS"
