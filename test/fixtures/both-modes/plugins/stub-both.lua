daukle.plugin{ api = 1, uses = { "region" } }

daukle.toolchain{
  name = "stub",
  generate = function(toolchain)
    return { ["generated.txt"] = "project " .. toolchain.project .. "\n" }
  end,
}

daukle.language{
  name = "stub",
  apply = function(consumer, resolved, text)
    return daukle.region(text, "// daukle:begin", "// daukle:end", "")
  end,
}
