daukle.plugin{ api = 1 }

local greeting = daukle.require("lib/greeting")

daukle.source{
  name = greeting.name,
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
