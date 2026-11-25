# Create a filtered source archive for release attachments.
param(
	[string]$Root = (Join-Path $PSScriptRoot '..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-GitBashArtifact
{
	param([string]$Name)

	$NormalizedName = $Name.TrimEnd([char[]]@('.', ' '))
	return $NormalizedName -match '^(CON|PRN|AUX|NUL|CLOCK\$|COM[1-9]|LPT[1-9])(?:\..*)?$'
}

$RootPath = [System.IO.Path]::GetFullPath($Root)
if (-not [System.IO.Directory]::Exists($RootPath))
{
	throw "[src-zip] Source directory does not exist: $RootPath"
}

$RootPrefix = $RootPath.TrimEnd([char[]]@('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
$ArchivePath = Join-Path -Path $RootPath -ChildPath 'src.zip'
$TemporaryArchivePath = Join-Path -Path $RootPath -ChildPath ".src.zip.$([guid]::NewGuid().ToString('N')).tmp"
$ExcludedDirectoryNames = @('.git', '.opencode', '.zcode', '.vs', '__pycache__', 'out')
$Entries = New-Object 'System.Collections.Generic.List[object]'
$DirectoriesToVisit = New-Object 'System.Collections.Generic.Stack[string]'
$DirectoriesToVisit.Push($RootPath)

while ($DirectoriesToVisit.Count -gt 0)
{
	$CurrentDirectory = $DirectoriesToVisit.Pop()
	foreach ($Path in [System.IO.Directory]::EnumerateFileSystemEntries($CurrentDirectory))
	{
		$Name = [System.IO.Path]::GetFileName($Path)
		$Attributes = [System.IO.File]::GetAttributes($Path)

		# Do not follow junctions or symlinks outside the source tree.
		if (($Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
		{
			continue
		}

		$RelativePath = $Path.Substring($RootPrefix.Length).Replace('\', '/')
		$IsDirectory = ($Attributes -band [System.IO.FileAttributes]::Directory) -ne 0
		if ($IsDirectory)
		{
			if (($ExcludedDirectoryNames -contains $Name) -or ($Name -like 'build-android-*'))
			{
				continue
			}

			$Entries.Add([pscustomobject]@{
				FullPath = $Path
				RelativePath = $RelativePath
				IsDirectory = $true
			})
			$DirectoriesToVisit.Push($Path)
			continue
		}

		if (($RelativePath -ieq 'src.zip') -or (Test-GitBashArtifact -Name $Name))
		{
			continue
		}

		$Entries.Add([pscustomobject]@{
			FullPath = $Path
			RelativePath = $RelativePath
			IsDirectory = $false
		})
	}
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName Microsoft.VisualBasic

$ZipStream = $null
$Zip = $null
$ArchiveCreationError = $null
try
{
	$ZipStream = [System.IO.File]::Open(
		$TemporaryArchivePath,
		[System.IO.FileMode]::CreateNew,
		[System.IO.FileAccess]::ReadWrite,
		[System.IO.FileShare]::None)
	$Zip = [System.IO.Compression.ZipArchive]::new(
		$ZipStream,
		[System.IO.Compression.ZipArchiveMode]::Create,
		$true)

	foreach ($Entry in $Entries)
	{
		if ($Entry.IsDirectory)
		{
			[void]$Zip.CreateEntry($Entry.RelativePath + '/', [System.IO.Compression.CompressionLevel]::Optimal)
			continue
		}

		$ZipEntry = $Zip.CreateEntry($Entry.RelativePath, [System.IO.Compression.CompressionLevel]::Optimal)
		$SourceStream = [System.IO.File]::OpenRead($Entry.FullPath)
		$EntryStream = $ZipEntry.Open()
		try
		{
			$SourceStream.CopyTo($EntryStream)
		}
		finally
		{
			$EntryStream.Dispose()
			$SourceStream.Dispose()
		}
	}
}
catch
{
	$ArchiveCreationError = $_
}
finally
{
	if ($null -ne $Zip)
	{
		$Zip.Dispose()
	}
	if ($null -ne $ZipStream)
	{
		$ZipStream.Dispose()
	}
}

if ($null -ne $ArchiveCreationError)
{
	if ([System.IO.File]::Exists($TemporaryArchivePath))
	{
		Remove-Item -LiteralPath $TemporaryArchivePath -Force -ErrorAction SilentlyContinue
	}
	Write-Host "[src-zip] ERROR: Archive creation failed: $($ArchiveCreationError.Exception.Message)" -ForegroundColor Red
	exit 1
}

try
{
	if ([System.IO.File]::Exists($ArchivePath))
	{
		[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
			$ArchivePath,
			[Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
			[Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
		Write-Host '[src-zip] Previous src.zip moved to the Recycle Bin.'
	}

	[System.IO.File]::Move($TemporaryArchivePath, $ArchivePath)
	Write-Host "[src-zip] Created $ArchivePath ($($Entries.Count) entries)."
}
catch
{
	Write-Host "[src-zip] ERROR: Failed to install the new archive: $($_.Exception.Message)" -ForegroundColor Red
	exit 1
}
finally
{
	if ([System.IO.File]::Exists($TemporaryArchivePath))
	{
		Remove-Item -LiteralPath $TemporaryArchivePath -Force -ErrorAction SilentlyContinue
	}
}
