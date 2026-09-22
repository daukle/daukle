daukle.plugin{ api = 1, uses = { "tool" } }

daukle.language{
  name = "ok",
  apply = function(consumer, resolved, text) return text end,
}
