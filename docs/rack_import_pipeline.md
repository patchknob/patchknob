# Rack Module Import Pipeline

`tools/import_vcv_rack_plugin.ps1` converts a Rack plugin's public metadata, model declarations, panel paths, and source locations into a PatchKnob rack-import manifest. It also invokes `cardinal_svg_packer` to pre-render every SVG in that plugin into a single PNG texture archive.

Run it from the project root after building `cardinal_svg_packer`:

```powershell
.\tools\import_vcv_rack_plugin.ps1 -PluginRoot .\vendor\Cardinal\plugins\Fundamental
```

The output is placed in `modules\<plugin>` and contains `<plugin>.dll`, `<plugin>.s24rack.json`, and `<plugin>.s24svgpak`. Regular CMake builds create the top-level `modules` directory. Each DLL exports the import manifest and asset-pack contract, so it is discoverable by the app without loading or parsing SVGs. To build a chosen non-Fundamental import or every remaining Cardinal plugin, use:

```powershell
.\tools\build_cardinal_modules.ps1 -Plugin Befaco
.\tools\build_cardinal_modules.ps1 -All
```

Fundamental is bundled by the main build. Add `-IncludeFundamental` when it should also be exported into `modules`. The generated DLL is an adapter library, not a misleading binary build of unmodified VCV source; compiling its original DSP requires the Rack SDK compatibility layer. The panel archive is immediately usable by the GPU renderer.
