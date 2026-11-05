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

daukle.source{
  name = marker.capability,
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
