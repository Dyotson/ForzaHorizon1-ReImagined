# Windows executable icon

`forza.ico` is the existing ReImagined logo from `docs/images/logo.png`, packaged
at 16, 20, 24, 32, 40, 48, 64, 128 and 256 pixels with transparency. The game,
PyInstaller launcher and Inno Setup installer share it. Keeping it inside
`forza-recomp` also includes it in the launcher's source bundle and build hash.

To regenerate from the repository root with Pillow installed:

```sh
python -c "from PIL import Image; Image.open('docs/images/logo.png').convert('RGBA').save('forza-recomp/resources/forza.ico', sizes=[(16,16),(20,20),(24,24),(32,32),(40,40),(48,48),(64,64),(128,128),(256,256)])"
```

The Windows CMake target compiles `app.rc.in` for both base-game and TU4 builds.
Linux builds do not compile Windows resources.
