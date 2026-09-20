daukle.config = {
  schema = 1,
  project = "forebay/two-ways",
  version = "1.0.0",
  modules = {},
  sources = { ["forebay/basekit"] = { kind = "path", path = "./producer-languages" } },
  plugins = { path = "./plugins/path.lua", npm = "./plugins/npm.lua" },
  consumers = {
    {
      id = "stub",
      language = "npm",
      file = "package.json",
      configuration = "dependencies",
      dependencies = { ["forebay/basekit"] = { version = "^5.0.0", modules = { "ir" } } },
    },
  },
}
