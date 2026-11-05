daukle.plugin{ api = 2, uses = {} }

assert(string.format("%d", 1) == "1")
assert(table.concat({ "a", "b" }) == "ab")
assert(math.floor(2.5) == 2)

daukle.source{
  name = "reader",
  load = function(project, block, base_dir)
    return { schema = 1, project = project, version = "1.0.0", modules = {} }
  end,
}
