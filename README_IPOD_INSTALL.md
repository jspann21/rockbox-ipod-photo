# Install the iPod Photo build

This installs the conservative `fa7ee27892` Rockbox build. The provisional
PP5020 forced PCM interrupt that blocked the preceding hardware test is
disabled. The Rockbox bootloader must already be installed on the iPod.

Open PowerShell in this repository and run:

```powershell
$ipod = Get-Volume -FileSystemLabel IPOD -ErrorAction Stop
if (@($ipod).Count -ne 1 -or -not $ipod.DriveLetter) { throw "Expected exactly one mounted volume labeled IPOD" }
$destination = "$($ipod.DriveLetter):\"
Expand-Archive -LiteralPath ".\dist\rockbox-ipodcolor-fa7ee27892.zip" -DestinationPath $destination -Force
Test-Path -LiteralPath "${destination}.rockbox\rockbox.ipod"
```

The last command must print `True`. On the currently connected machine, the
detected destination is `F:\`.

The equivalent direct command is:

```powershell
Expand-Archive -LiteralPath ".\dist\rockbox-ipodcolor-fa7ee27892.zip" -DestinationPath "F:\" -Force
```

Use the direct form only after confirming that `F:` is still the iPod:

```powershell
Get-Volume -DriveLetter F
```

After extraction completes, safely eject the iPod from Windows and boot it.
The firmware should report version `fa7ee27892-260824`. The expected SHA-256
of the installable `rockbox.ipod` is
`849c539df7badb3dad7c6152f05bbfe729687c391dfceb7de6c6931ee7a7ab8d`.

After completing the combined playback, storage-sleep, wake, and USB test pass,
open `Debug > View PP5020 performance` and press Select to save the single
snapshot. The resulting file is `.rockbox\pp5020-perf.log`; copy that file back
during the next connection.
