# Pd-Host for VCV Rack
## by EL LOCUS SOLUS

[Pure Data](https://puredata.info) Host for [VCV Rack](https://vcvrack.com/) containing:

- 6 inputs
- 6 outputs
- 6 knobs
- 6 lights (RGB LEDs)
- 6 switches with RGB LEDs

The Pd-Host repository lives at <https://github.com/EL-LOCUS-SOLUS/vcv-pdhost>. **"EL Locus Solus"** is run by yours truly, Alexandre Torres Porres, and it organizes cultural events/concerts and computer music courses. I'm a Pd developer and have libraries/tutorials for Pd and contribute to its code and documentation. Please support me on <https://www.patreon.com/porres> if you like this project.

This works is based on an old and abandoned project called Prototype, which used to run Pd patches via libpd as well as other engines in VCV1. 

You don't need Pure Data installed in your system, but of course it's nice if you do as you can call it via VCV to edit patches.


## Contributors

- [Wes Milholen](https://grayscale.info/): panel design
- [Andrew Belt](https://github.com/AndrewBelt): host code
- [CHAIR](https://chair.audio) (Clemens Wegener, Max Neupert): libpd support for the Prototype module
- Porres forked from the old prototype module, stripped down to Pd only, ported to VCV2 and made some other changes like adding multi instance support to create this project.
- Jeremy Wentworth general help and guidance


## Building

First set up your build environment as described in: https://vcvrack.com/manual/Building and prepare according to your system as below:


### Windows
```bash
pacman -S mingw-w64-x86_64-premake
```

### Mac
```bash
brew install premake
```

### Ubuntu 16.04+
```bash
sudo apt install premake4
```

### Arch Linux
```bash
sudo pacman -S premake
```

--

#### With Rack-SDK's folder in the same level as Pd-Host's and just run:

```bash
make dep
make
```

that's should be it...