daukle.plugin{ api = 1, uses = { "env" } }

local home = daukle.env("PATH")

daukle.language{
  name = "brewfile",
  apply = function(consumer, resolved, text)
    return tostring(home ~= nil) .. "\n"
  end,
}
