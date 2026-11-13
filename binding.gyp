{
  "targets": [
    {
      "target_name": "ctp",
      "sources": [
        "src/addon.cc",
        "src/native/risk.cc",
        "src/native/arm.cc",
        "src/native/channel.cc",
        "src/native/gbk.cc",
        "src/native/mdspi.cc",
        "src/native/mdapi.cc",
        "src/native/traderapi.cc",
        "src/generated/layout.gen.cc",
        "src/generated/traderspi.gen.cc",
        "src/generated/traderreq.gen.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "ctpsdk"
      ],
      "defines": [
        "NAPI_VERSION=8",
        "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "conditions": [
        ["OS=='win'", {
          "library_dirs": [ "<(module_root_dir)/ctpsdk/windows" ],
          "libraries": [
            "thostmduserapi_se.lib",
            "thosttraderapi_se.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1
            }
          },
          "copies": [
            {
              "destination": "<(PRODUCT_DIR)",
              "files": [
                "<(module_root_dir)/ctpsdk/windows/thostmduserapi_se.dll",
                "<(module_root_dir)/ctpsdk/windows/thosttraderapi_se.dll"
              ]
            }
          ]
        }],
        ["OS=='linux'", {
          # The CTP .so files carry no SONAME, so linking them by path would bake
          # that build-host path into DT_NEEDED. Link by bare file name instead and
          # resolve only via $ORIGIN (the .so files sit next to the addon).
          "library_dirs": [ "<(module_root_dir)/ctpsdk/linux" ],
          "libraries": [
            "-l:thostmduserapi_se.so",
            "-l:thosttraderapi_se.so",
            "-Wl,-rpath,'$$ORIGIN'"
          ],
          "cflags_cc": [ "-std=c++17", "-fexceptions" ],
          "copies": [
            {
              "destination": "<(PRODUCT_DIR)",
              "files": [
                "<(module_root_dir)/ctpsdk/linux/thostmduserapi_se.so",
                "<(module_root_dir)/ctpsdk/linux/thosttraderapi_se.so"
              ]
            }
          ]
        }],
        ["OS=='mac'", {
          "include_dirs": [
            "<(module_root_dir)/ctpsdk/macos/thostmduserapi_se.framework/Versions/A/Headers",
            "<(module_root_dir)/ctpsdk/macos/thosttraderapi_se.framework/Versions/A/Headers"
          ],
          "libraries": [
            "-F<(module_root_dir)/ctpsdk/macos",
            "-framework thostmduserapi_se",
            "-framework thosttraderapi_se",
            "-liconv",
            "-Wl,-rpath,@loader_path",
            "-Wl,-rpath,<(module_root_dir)/ctpsdk/macos"
          ],
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.15"
          }
        }]
      ]
    }
  ]
}
