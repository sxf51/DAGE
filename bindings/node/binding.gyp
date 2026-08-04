{
  "targets": [{
    "target_name": "dage_node",
    "sources": ["native.cpp"],
    "include_dirs": ["<!@(node -p \"require('node-addon-api').include\")", "../../include"],
    "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
    "cflags_cc": ["-Wall", "-Wextra", "-Werror", "-fexceptions"],
    "conditions": [
      ["OS=='win'", {
        "libraries": ["kernel32.lib"],
        "msvs_settings": {
          "VCCLCompilerTool": {
            "ExceptionHandling": 1,
            "WarningLevel": 4,
            "WarnAsError": "true"
          }
        }
      }],
      ["OS!='win'", {"libraries": ["-ldl"]}]
    ]
  }]
}
