daukle.plugin{ api = 2, uses = { "region" } }

local borrowed = daukle.write

daukle.language{
  name = "modular-greedy",
  apply = function(consumer, resolved, text) return text end,
}
