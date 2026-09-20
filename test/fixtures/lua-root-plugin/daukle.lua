daukle.source{
  name = "invented",
  load = function(project, block, base_dir)
    return {
      schema = 1,
      project = project,
      version = "5.0.0",
      modules = { ir = { greet = {} } },
    }
  end,
}

daukle.config = {
  schema = 1,
  project = "forebay/lua-root-plugin",
  version = "1.0.0",
  modules = {},
  plugins = { greet = "./plugins/greet.lua" },
  sources = { ["forebay/basekit"] = { kind = "invented" } },
  consumers = {
    {
      id = "text",
      language = "greet",
      file = "greet.txt",
      configuration = "deps",
      dependencies = { ["forebay/basekit"] = { version = "^5.0.0", modules = { "ir" } } },
    },
  },
}
