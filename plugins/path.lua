daukle.plugin{ api = 1, uses = { "read", "parse" } }

daukle.source{
  name = "path",
  load = function(project, block, base_dir)
    if type(block.path) ~= "string" then
      error("sources." .. project .. " has no \"path\"")
    end
    local file = block.path .. "/daukle.toml"
    return daukle.parse(daukle.read(file), file)
  end,
}
