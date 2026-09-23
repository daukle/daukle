daukle.plugin{ api = 1, uses = {} }

local greeting = daukle.require("lib/greeting")

daukle.language{
  name = greeting.name,
  apply = function(consumer, resolved, text)
    return greeting.render(resolved)
  end,
}
