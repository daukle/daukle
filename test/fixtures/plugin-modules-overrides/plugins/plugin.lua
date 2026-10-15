daukle.plugin{
  api = 2,
  uses = {},
  requires = {
    helper = { url = "https://example.invalid/helper.lua", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" },
  },
}

daukle.toolchain{
  name = "overridden",
  generate = function(context) return {} end,
}
