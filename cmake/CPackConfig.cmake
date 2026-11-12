set(CPACK_PACKAGE_VENDOR "GNU Radio")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "GNU Radio 4 control plane")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
# without this CPack substitutes its own placeholder licence into the installer
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CONTACT "admin@gnuradio.org")
if(NOT CPACK_PACKAGE_NAME)
  set(CPACK_PACKAGE_NAME "gnuradio4-control-plane")
endif()
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

include(CPackComponent)

cpack_add_component(
  CORE
  DISPLAY_NAME "Runtime"
  DESCRIPTION "GNU Radio 4 control plane server and command-line client"
  REQUIRED)
cpack_add_component(DEVELOP DISPLAY_NAME "Development files"
                    DESCRIPTION "Headers and static libraries of the control plane core and client")
# MSVC only in practice: GrInstall.cmake puts each target's PDB here, and
# nothing else produces separate debug information.
cpack_add_component(SYMBOLS DISPLAY_NAME "Debug symbols"
                    DESCRIPTION "Program database files for debugging the installed binaries")

# APPLE implies UNIX, so macOS must be excluded here or it selects the Debian
# generator and `--target package` tries to build a .deb on a Mac.
if(UNIX AND NOT APPLE)
  if(NOT CPACK_GENERATOR)
    set(CPACK_GENERATOR "DEB")
  endif()

  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  # When installing to a non-standard prefix (e.g. /opt/gnuradio4), dpkg-shlibdeps
  # cannot find the package's own private libraries. Point it to the install lib dir.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${CMAKE_INSTALL_PREFIX}/lib")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(CPACK_DEBIAN_DEBUGINFO_PACKAGE ON)
  endif()
  # GR 4's own libraries are left to the package that ships them, so depend on it
  # rather than on dpkg-shlibdeps finding an owner for them
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "gnuradio4-core")
endif()

if(APPLE)
  set(CPACK_GENERATOR "productbuild")
  # gr_configure_productbuild_installer() comes from gr-core's installed
  # GrProductBuild.cmake, which gnuradio4Config.cmake includes on APPLE
  set(CPACK_COMPONENTS_ALL CORE DEVELOP Unspecified)

  # the identifier identifies gr-control-plane to pkgutil across releases: never
  # reuse it in another GR installer and never change it between versions
  gr_configure_productbuild_installer(
    IDENTIFIER
    "org.gnuradio.gr4.control-plane"
    PACKAGES
    "gr-control-plane=${CPACK_PACKAGE_VERSION}"
    REGISTER_COMPONENT
    CORE
    PRESELECTED_COMPONENTS
    CORE)
endif()

if(WIN32)
  set(CPACK_GENERATOR "INNOSETUP")
  set(CPACK_INNOSETUP_USE_CMAKE_BOOL_FORMAT ON)
  set(CPACK_INNOSETUP_USE_MODERN_WIZARD ON)
  set(CPACK_INNOSETUP_ALLOW_CUSTOM_DIRECTORY ON)
  # fallback only: gr_configure_innosetup_installer() takes the install root from
  # the shared manifest when a GR 4 installation already exists
  set(CPACK_INNOSETUP_INSTALL_ROOT "{autopf}")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "GNURadio4")
  # The Start Menu folder is GR_START_MENU, defaulted and applied by
  # GrInnoSetup.cmake below so that every GR 4 package agrees on it.
  # without an explicit list CPack builds a monolithic installer and ignores
  # /COMPONENTS, which the superbuild installer uses to select DEVELOP and
  # SYMBOLS. SYMBOLS is Windows-only: there are no PDBs to ship elsewhere.
  set(CPACK_COMPONENTS_ALL CORE DEVELOP SYMBOLS Unspecified)

  # gr_configure_innosetup_installer() comes from find_package(gnuradio4)
  # the AppId identifies gr-control-plane to Windows across releases: never reuse
  # it in another GR installer and never change it between versions
  # the leading unbalanced { is required, inno installers are scripted
  # in pascal, evidently they use asymetrical escapes
  gr_configure_innosetup_installer(
    APP_ID
    "{{533d8cc5-ecff-4132-b1b5-0ffa195a96a4}"
    PACKAGES
    "gr-control-plane=${CPACK_PACKAGE_VERSION}"
    PRESELECTED_COMPONENTS
    CORE)
endif()

include(CPack)
