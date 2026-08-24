# Install the iPod Photo build

This installs the conservative `0062a3c3a3` Rockbox build. The provisional
PP5020 forced PCM interrupt that blocked the preceding hardware test is
disabled. This build reached Rockbox on the installed A1099 during its initial
boot smoke test and passed the first storage, USB-integrity, and playback
transition checks. The Rockbox bootloader must already be installed on the iPod.

Open PowerShell in this repository and run:

```powershell
$ipod = Get-Volume -FileSystemLabel IPOD -ErrorAction Stop
if (@($ipod).Count -ne 1 -or -not $ipod.DriveLetter) { throw "Expected exactly one mounted volume labeled IPOD" }
$destination = "$($ipod.DriveLetter):\"
Expand-Archive -LiteralPath ".\dist\rockbox-ipodcolor-0062a3c3a3.zip" -DestinationPath $destination -Force
Test-Path -LiteralPath "${destination}.rockbox\rockbox.ipod"
```

The last command must print `True`. On the currently connected machine, the
detected destination is `F:\`.

The equivalent direct command is:

```powershell
Expand-Archive -LiteralPath ".\dist\rockbox-ipodcolor-0062a3c3a3.zip" -DestinationPath "F:\" -Force
```

Use the direct form only after confirming that `F:` is still the iPod:

```powershell
Get-Volume -DriveLetter F
```

After extraction completes, safely eject the iPod from Windows and boot it.
The firmware should report version `0062a3c3a3-260824`. The expected SHA-256
of the installable `rockbox.ipod` is
`13e602fa04e0fda58c809bbe42979a33414c7b9b30959b5e0d54d4db0ac09b2c`.

After completing the combined playback, storage-sleep, wake, and USB test pass,
open `Debug > View PP5020 performance` and press Select to save the single
snapshot. The resulting file is `.rockbox\pp5020-perf.log`; copy that file back
during the next connection.
