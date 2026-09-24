daukle.plugin{
  api = 1,
  uses = {},
  requires = {
    provider = {
      url = "https://example.invalid/never-fetched.lua",
      sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    },
  },
}

local marker = daukle.require("provider:lib/marker")

daukle.language{
  name = marker.capability,
  apply = function(consumer, resolved, text)
    return text
  end,
}
