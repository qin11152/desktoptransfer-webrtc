Third-party binary libraries are organized by platform.

- linux: Linux static/shared libraries used by the current build.
- windows: Placeholder for Windows import libraries, static libraries, and DLL staging.
- macos: Placeholder for macOS static libraries, dynamic libraries, and frameworks.

Keep common headers under 3rd/include and place platform-specific binaries only in the matching subdirectory.