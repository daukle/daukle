daukle.plugin{ api = 1, uses = { "region" } }

daukle.language{
  name = "gradle",
  apply = function(consumer, resolved, text)
    if consumer.configuration == nil or consumer.configuration == "" then
      error("consumer has no \"configuration\" for the gradle language plugin")
    end
    local lines = {}
    for index = 1, #resolved do
      local entry = resolved[index]
      local coordinate = entry.block.coordinate
      if type(coordinate) ~= "string" then
        error("modules." .. entry.module .. ".gradle has no \"coordinate\"")
      end
      lines[index] = consumer.configuration .. " \"" .. coordinate .. "\""
    end
    return daukle.region(text, "// daukle:begin", "// daukle:end", table.concat(lines, "\n"))
  end,
}
