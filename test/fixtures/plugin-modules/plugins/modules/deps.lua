daukle.plugin{ api = 2, uses = { "region" } }

daukle.language{
  name = "modular-deps",
  apply = function(consumer, resolved, text) return text end,
}
