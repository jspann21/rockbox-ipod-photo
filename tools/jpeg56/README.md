# JPEG ARM IDCT + full-range LCD hardware check

This branch uses one installed build for both measurements. It adds no workflow
and does not require repeated intermediate firmware builds.

Reuse the same eight JPEGs from the previous `jpegbench` corpus.

## Reference pass

1. Delete any old `.rockbox/jpeg56.csv`.
2. Create empty files `.rockbox/jpeg56.enabled` and
   `.rockbox/jpeg56.reference`.
3. Set Image Viewer to **Colour** and **No dithering**.
4. View each of the eight corpus JPEGs once.

This pass uses the legacy C IDCT and normal framebuffer/LCD update path.

## Accelerated pass

1. Exit Image Viewer.
2. Delete `.rockbox/jpeg56.reference`, leaving `jpeg56.enabled`.
3. Reopen Image Viewer and view the same eight JPEGs once.
4. For both 220x176 files, verify correct orientation, RGB channels, black and
   white levels, and no tearing or partial rows.
5. Copy `.rockbox/jpeg56.csv` to the computer and run:

```sh
python3 tools/jpeg56/check_log.py /path/to/jpeg56.csv
```

The accelerated pass uses Rockbox's classic-ARM vertical JPEG IDCT passes and
the full-range direct LCD path for exact-screen 4:2:0 images.

After acceptance, remove both sentinels. The temporary logger/reference
selector will be removed before the branch is squashed into production form.
