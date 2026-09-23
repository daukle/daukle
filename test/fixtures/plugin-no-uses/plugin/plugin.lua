daukle.plugin{ api = 1 }

local greeting = daukle.require("lib/greeting")

daukle.language{
  name = greeting.name,
  apply = function(consumer, resolved, text)
    return text
  end,
}
