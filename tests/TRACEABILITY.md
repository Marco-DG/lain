# Lain conformance-suite traceability matrix

_Generated map of every test → spec chapter. Drives the suite reorg + the
spec↔compiler reconciliation. Model: `spec/` = contract, tests = live
conformance suite, compiler = implementation; reconcile in sprints, tests as arbiter._

## Naming law (target)

- `feature_scenario_pass.ln` / `feature_scenario_fail.ln` — **every standalone test** (enforced).
- Every `_fail` carries `// EXPECT: [EXXX]`. Every test: `// spec: §N` header.
- No `char_` prefix (the directory conveys the feature); no loose files at `tests/` root.

**Two exemptions from the `_pass`/`_fail` law** (they aren't pass/fail tests):
- **Fixtures** — helper modules `import`ed by other tests (e.g. `tests/stdlib/dummy.ln`).
  Their name is the import path, so it must stay stable.
- **Snapshots** — `emit/*` tests with a `.grep` sidecar that asserts the emitted-C
  *structure*. The `.grep` bakes in the mangled filename, so the name must stay stable.

**Enforcing `_pass` surfaced two latent broken-C findings** (an "other" test skips the
gcc-check, so their broken emitted C went unnoticed) — now tracked in `run_tests.sh`
`GCC_CHECK_SKIP` and to be fixed in reconciliation: `comptime_if_pass` (sentinel
`*u8[:0]` printf arg emits fat `Slice_u8_0`, not `const char*` — §17 C-string interop)
and `test_mem_pass` (`std/mem` `malloc` binding conflict — §17/§20).

## Target structure (one dir per spec chapter)
```
tests/types/        (07)  structs, enums, ADTs, arrays, slices, pointers, casts
tests/expressions/  (08)  operators, literals, widening, comparisons
tests/statements/   (09)  control flow, defer, loops, blocks
tests/declarations/ (10)  var/const bindings, function declarations
tests/ownership/    (11)  move, borrow, linear types, NLL, two-phase
tests/functions/    (12)  func/proc, purity, termination, fn-pointers
tests/vra/          (13)  bounds, overflow, refinement, invariants, in-guards, constraints
tests/generics/     (14)  monomorphization, comptime, type params
tests/match/        (15)  case, union `T|marker`, niche, optional
tests/modules/      (16)  import, qualified access, visibility
tests/interop/      (17)  extern C types/functions
tests/unsafe/       (18)  unsafe blocks, deref, address-of
tests/memory/       (19)  initialization / definite assignment
tests/stdlib/       (20)  std library smoke tests
tests/codegen/      (IMPL)  ABI/emit snapshots — implementation, not spec behavior
tests/examples/     (INTEG)  end-to-end integration programs
```

## Coverage & gaps

| chapter | dir | tests | status |
|:--|:--|--:|:--|
| 07 | types | 69 | ok |
| 08 | expressions | 8 | ok |
| 09 | statements | 9 | ok |
| 10 | declarations | 0 | **GAP — thin/none** |
| 11 | ownership | 85 | ok |
| 12 | functions | 26 | ok |
| 13 | vra | 108 | ok |
| 14 | generics | 13 | ok |
| 15 | match | 29 | ok |
| 16 | modules | 2 | **GAP — thin/none** |
| 17 | interop | 0 | **GAP — thin/none** |
| 18 | unsafe | 3 | ok |
| 19 | memory | 21 | ok |
| 20 | stdlib | 12 | ok |
| IMPL | codegen | 7 | ok |
| INTEG | examples | 18 | ok |
| ?? | (unmapped) | 29 | **needs manual triage** |

**TOTAL 439 tests.** Gaps to fill: lexical(05), basics(06), declarations(10), interop(17) have ~0; modules(16), unsafe(18) are thin.

## Issues to fix in the reorg

- **char-prefix**: 96
- **grab-bag**: 90
- **no-suffix**: 61
- **no-EXPECT**: 23

## Full inventory (current → target)


### 07 types (69)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_array_elem_narrow_implicit_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_array_literal_length_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_dependent_array_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_enum_arithmetic_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_enum_as_int_cast_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_float_int_implicit_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_int_to_ptr_implicit_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_local_array_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_mixed_float_int_arith_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_multidim_array_fail.ln` | fail | E100 | char-prefix;grab-bag |
| `tests/characterization/char_null_zero_ptr_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_ptr_elem_mismatch_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_ptr_field_write_fail.ln` | fail | E009 | char-prefix;grab-bag |
| `tests/characterization/char_ptr_int_compare_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_recursive_struct_fail.ln` | fail | E104 | char-prefix;grab-bag |
| `tests/characterization/char_return_local_array_slice_fail.ln` | fail | E010 | char-prefix;grab-bag |
| `tests/characterization/char_return_local_slice_fail.ln` | fail | E010 | char-prefix;grab-bag |
| `tests/characterization/char_slice_codegen_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_struct_arithmetic_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_struct_field_reassign_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_struct_slice_mutation_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_struct_slice_wrong_field_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_struct_type_confusion_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_subslice_oob_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_while_lt_small_array_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/core/unsafe_adt_fail.ln` | fail | — | no-EXPECT |
| `tests/core/unsafe_adt_pass.ln` | pass | — | — |
| `tests/types/adt.ln` | other | — | no-suffix |
| `tests/types/array_literal_pass.ln` | pass | — | — |
| `tests/types/arrays.ln` | other | — | no-suffix |
| `tests/types/bool_test.ln` | other | — | no-suffix |
| `tests/types/cast_pointer_fail.ln` | fail | E012 | — |
| `tests/types/cast_test.ln` | other | — | no-suffix |
| `tests/types/char_test.ln` | other | — | char-prefix;no-suffix |
| `tests/types/destructuring.ln` | other | — | no-suffix |
| `tests/types/enum_exhaustive.ln` | other | — | no-suffix |
| `tests/types/enums.ln` | other | — | no-suffix |
| `tests/types/exhaustive_fail.ln` | fail | — | no-EXPECT |
| `tests/types/float_test.ln` | other | — | no-suffix |
| `tests/types/int_alias_pass.ln` | pass | — | — |
| `tests/types/integer_iN_arbitrary_pass.ln` | pass | — | — |
| `tests/types/integer_types.ln` | other | — | no-suffix |
| `tests/types/match_borrow_mut_fail.ln` | fail | E004 | — |
| `tests/types/overflow_assign_fail.ln` | fail | E086 | — |
| `tests/types/overflow_boundary_fail.ln` | fail | E086 | — |
| `tests/types/overflow_boundary_safe_pass.ln` | pass | — | — |
| `tests/types/overflow_callarg_fail.ln` | fail | E086 | — |
| `tests/types/overflow_field_assign_fail.ln` | fail | E086 | — |
| `tests/types/overflow_operators_pass.ln` | pass | — | — |
| `tests/types/overflow_return_fail.ln` | fail | E086 | — |
| `tests/types/overflow_wrap_explicit_pass.ln` | pass | — | — |
| `tests/types/p1a_decay_test.ln` | other | — | no-suffix |
| `tests/types/packed_mixed_widths_pass.ln` | pass | — | — |
| `tests/types/packed_struct_pass.ln` | pass | — | — |
| `tests/types/packed_struct_reconstruct_pass.ln` | pass | — | — |
| `tests/types/paradigm_b_overflow_fail.ln` | fail | E086 | — |
| `tests/types/paradigm_b_widening_pass.ln` | pass | — | — |
| `tests/types/refinement_alias_int_pass.ln` | pass | — | — |
| `tests/types/refinement_type_alias_fail.ln` | fail | E086 | — |
| `tests/types/refinement_type_alias_pass.ln` | pass | — | — |
| `tests/types/string_slices.ln` | other | — | no-suffix |
| `tests/types/struct_partial_init_fail.ln` | fail | E012 | — |
| `tests/types/struct_type_mismatch_fail.ln` | fail | E012 | — |
| `tests/types/structs.ln` | other | — | no-suffix |
| `tests/types/sub_slice_basic_pass.ln` | pass | — | — |
| `tests/types/sub_slice_bounds_fail.ln` | fail | — | no-EXPECT |
| `tests/types/vra_l1_autosize_pass.ln` | pass | — | — |
| `tests/types/vra_l2_int_concretize_pass.ln` | pass | — | — |
| `tests/types/wrong_arg_count_fail.ln` | fail | E012 | — |

### 08 expressions (8)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_float_int_literal_promote_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_i64_literal_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/core/bitwise_test.ln` | other | — | no-suffix |
| `tests/core/compound_assign_test.ln` | other | — | no-suffix |
| `tests/core/numeric_literals.ln` | other | — | no-suffix |
| `tests/parser/attribute_fast_math_pass.ln` | pass | — | — |
| `tests/parser/attribute_multiple_pass.ln` | pass | — | — |
| `tests/parser/attribute_unknown_fail.ln` | fail | E103 | — |

### 09 statements (9)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_consume_before_return_fail.ln` | fail | E003 | char-prefix;grab-bag |
| `tests/characterization/char_while_lt_wide_badpad_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/core/control_flow.ln` | other | — | no-suffix |
| `tests/core/defer_basic.ln` | other | — | no-suffix |
| `tests/core/defer_loop.ln` | other | — | no-suffix |
| `tests/core/defer_return.ln` | other | — | no-suffix |
| `tests/core/for_two_var.ln` | other | — | no-suffix |
| `tests/core/panic_no_defer.ln` | other | — | no-suffix |
| `tests/core/while_loop.ln` | other | — | no-suffix |

### 11 ownership (85)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/borrowck/block_scoped_borrow_release_pass.ln` | pass | — | — |
| `tests/borrowck/nll_while.ln` | other | — | no-suffix |
| `tests/borrowck/return_borrow_var_param_pass.ln` | pass | — | — |
| `tests/borrowck/return_slice_borrows_active_pass.ln` | pass | — | — |
| `tests/borrowck/return_slice_borrows_param_pass.ln` | pass | — | — |
| `tests/borrowck/struct_field_dangling_fail.ln` | fail | E010 | — |
| `tests/borrowck/struct_field_param_pass.ln` | pass | — | — |
| `tests/characterization/char_move_needs_mov_fail.ln` | fail | E007 | char-prefix;grab-bag |
| `tests/characterization/char_mut_plus_nested_shared_fail.ln` | fail | E004 | char-prefix;grab-bag |
| `tests/characterization/char_recv_plus_shared_fail.ln` | fail | E004 | char-prefix;grab-bag |
| `tests/characterization/char_scalar_var_shared_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_two_shared_same_call_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_use_after_move_fail.ln` | fail | E001 | char-prefix;grab-bag |
| `tests/core/match_borrow_pass.ln` | pass | — | — |
| `tests/core/two_phase_borrow_pass.ln` | pass | — | — |
| `tests/safety/borrow/block_nll_pass.ln` | pass | — | — |
| `tests/safety/borrow/defer_consume_pass.ln` | pass | — | — |
| `tests/safety/borrow/rich_diagnostics_fail.ln` | fail | E003 | — |
| `tests/safety/ownership/00_immutability.ln` | other | — | no-suffix |
| `tests/safety/ownership/01_move_basic.ln` | other | — | no-suffix |
| `tests/safety/ownership/02_borrow_basic.ln` | other | — | no-suffix |
| `tests/safety/ownership/block_scope.ln` | other | — | no-suffix |
| `tests/safety/ownership/block_scope_fail.ln` | fail | E003 | — |
| `tests/safety/ownership/borrow_conflict_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/borrow_fail.ln` | fail | E008 | — |
| `tests/safety/ownership/borrow_pass.ln` | pass | — | — |
| `tests/safety/ownership/borrow_valid.ln` | other | — | no-suffix |
| `tests/safety/ownership/close_without_mov_fail.ln` | fail | E007 | — |
| `tests/safety/ownership/coarg_alias_member_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/coarg_disjoint_fields_pass.ln` | pass | — | — |
| `tests/safety/ownership/concat_pass.ln` | pass | — | — |
| `tests/safety/ownership/cross_stmt_borrow_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/cross_stmt_borrow_write_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/defer_consume_fail.ln` | fail | E012 | — |
| `tests/safety/ownership/defer_consume_pass.ln` | pass | — | — |
| `tests/safety/ownership/defer_double_consume_fail.ln` | fail | E002 | — |
| `tests/safety/ownership/defer_double_defer_fail.ln` | fail | E002 | — |
| `tests/safety/ownership/direct_var_borrow_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/direct_var_borrow_pass.ln` | pass | — | — |
| `tests/safety/ownership/div_index_pass.ln` | pass | — | — |
| `tests/safety/ownership/double_close_fail.ln` | fail | E002 | — |
| `tests/safety/ownership/double_consume_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/ownership/e087_cross_param_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/ownership/e087_literal_bound_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/ownership/elif_borrow_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/ownership/elif_borrow_pass.ln` | pass | — | — |
| `tests/safety/ownership/field_consume_fail.ln` | fail | E003 | — |
| `tests/safety/ownership/field_consume_pass.ln` | pass | — | — |
| `tests/safety/ownership/field_partial_move_fail.ln` | fail | E008 | — |
| `tests/safety/ownership/forgot_close_fail.ln` | fail | E003 | — |
| `tests/safety/ownership/immutable_fail.ln` | fail | E009 | — |
| `tests/safety/ownership/len_minus1_pass.ln` | pass | — | — |
| `tests/safety/ownership/linear_copy_double_free_fail.ln` | fail | E002 | — |
| `tests/safety/ownership/loop_reassign_consume_pass.ln` | pass | — | — |
| `tests/safety/ownership/mod_index_pass.ln` | pass | — | — |
| `tests/safety/ownership/mov_local_pointer_pass.ln` | pass | — | — |
| `tests/safety/ownership/mov_syntax.ln` | other | — | no-suffix |
| `tests/safety/ownership/move_then_use_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/ownership/multi_var_param_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/multi_var_param_pass.ln` | pass | — | — |
| `tests/safety/ownership/nll_last_use_pass.ln` | pass | — | — |
| `tests/safety/ownership/nll_loop_borrow_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/nll_still_active_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/owner_reassign_after_release_pass.ln` | pass | — | — |
| `tests/safety/ownership/owner_reassign_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/ownership.ln` | other | — | no-suffix |
| `tests/safety/ownership/reborrow_chain_pass.ln` | pass | — | — |
| `tests/safety/ownership/reborrow_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/reborrow_pass.ln` | pass | — | — |
| `tests/safety/ownership/reborrow_same_stmt_fail.ln` | fail | E004 | — |
| `tests/safety/ownership/return_var_dangling_fail.ln` | fail | E010 | — |
| `tests/safety/ownership/return_var_local_fail.ln` | fail | E010 | — |
| `tests/safety/ownership/reverse_pass.ln` | pass | — | — |
| `tests/safety/ownership/sequential_borrow_pass.ln` | pass | — | — |
| `tests/safety/ownership/sized_literal_pass.ln` | pass | — | — |
| `tests/safety/ownership/slide_pass.ln` | pass | — | — |
| `tests/safety/ownership/subslice_pass.ln` | pass | — | — |
| `tests/safety/ownership/two_phase_borrow_fail.ln` | fail | E008 | — |
| `tests/safety/ownership/u1_index_pass.ln` | pass | — | — |
| `tests/safety/ownership/use_after_move_fail.ln` | fail | E001 | — |
| `tests/safety/ownership/var_prim_forward_pass.ln` | pass | — | — |
| `tests/safety/ownership/var_prim_read_pass.ln` | pass | — | — |
| `tests/safety/ownership/var_prim_write_pass.ln` | pass | — | — |
| `tests/safety/struct_linear_field_fail.ln` | fail | E001 | — |
| `tests/safety/struct_linear_field_no_mov_fail.ln` | fail | E083 | — |

### 12 functions (26)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_all_paths_return_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_decreasing_both_paths_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_decreasing_ok_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_decreasing_per_path_fail.ln` | fail | E082 | char-prefix;grab-bag |
| `tests/characterization/char_missing_return_fail.ln` | fail | E018 | char-prefix;grab-bag |
| `tests/characterization/char_proc_div_by_zero_fail.ln` | fail | E015 | char-prefix;grab-bag |
| `tests/characterization/char_return_addr_local_fail.ln` | fail | E010 | char-prefix;grab-bag |
| `tests/core/func_proc.ln` | other | — | no-suffix |
| `tests/core/functions.ln` | other | — | no-suffix |
| `tests/fnptr/fnptr_arity_fail.ln` | fail | E122 | — |
| `tests/fnptr/fnptr_basic_pass.ln` | pass | — | — |
| `tests/fnptr/fnptr_call_arity_fail.ln` | fail | E123 | — |
| `tests/fnptr/fnptr_hof_pass.ln` | pass | — | — |
| `tests/fnptr/fnptr_proc_target_pass.ln` | pass | — | — |
| `tests/fnptr/fnptr_ret_fail.ln` | fail | E122 | — |
| `tests/fnptr/fnptr_totality_fail.ln` | fail | E122 | — |
| `tests/safety/purity/bounded_while.ln` | other | — | no-suffix |
| `tests/safety/purity/bounded_while_bad_fail.ln` | fail | E082 | — |
| `tests/safety/purity/div_by_zero_func_fail.ln` | fail | E015 | — |
| `tests/safety/purity/func_calls_proc_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/purity/func_pure_pass.ln` | pass | — | — |
| `tests/safety/purity/func_self_recursion_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/purity/mutual_recursion_fail.ln` | fail | E011 | — |
| `tests/safety/purity/purity_fail.ln` | fail | E011 | — |
| `tests/safety/purity/repro_purity_fail.ln` | fail | E011 | — |
| `tests/safety/purity/while_in_func_fail.ln` | fail | E011 | — |

### 13 vra (108)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_aggregate_member_alias_fail.ln` | fail | E004 | char-prefix;grab-bag |
| `tests/characterization/char_alias_param_range_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_alias_param_seeds_bounds_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_alias_reassign_range_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_alias_return_range_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_alias_valid_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_arg_literal_overflow_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_array_literal_element_overflow_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_constraint_mutation_nested_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_constraint_mutation_oob_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_enum_payload_float_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_enum_payload_overflow_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_guard_mutation_nested_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_guard_mutation_oob_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_guarded_compound_index_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_in_guard_dynamic_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_len_param_refinement_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_literal_i64_overflow_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_nonlinear_index_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_overflow_static_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_refinement_narrow_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_return_refinement_proven_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_return_refinement_unproven_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_struct_field_literal_overflow_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_struct_field_refinement_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_struct_field_refinement_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_subslice_inbounds_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_vra_len_guard_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_vra_loop_lower_bound_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_vra_reverse_plain_slice_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_while_lt_signed_index_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_while_lt_unbounded_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/range/affine_loop_post_pass.ln` | pass | — | — |
| `tests/range/affine_step_neg_pass.ln` | pass | — | — |
| `tests/range/branch_relation_join_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/affine_recap_break_fail.ln` | fail | E085 | — |
| `tests/safety/bounds/affine_recap_multistep_fail.ln` | fail | E085 | — |
| `tests/safety/bounds/bounds_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/bounds_pass.ln` | pass | — | — |
| `tests/safety/bounds/dynamic_len_known_idx_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/equation_constraints.ln` | other | — | no-suffix |
| `tests/safety/bounds/equation_constraints_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/field_refinement_read_pass.ln` | pass | — | — |
| `tests/safety/bounds/in_condition_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/in_condition_pass.ln` | pass | — | — |
| `tests/safety/bounds/in_keyword.ln` | other | — | no-suffix |
| `tests/safety/bounds/in_keyword_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/interprocedural_return_range_pass.ln` | pass | — | — |
| `tests/safety/bounds/or_guard_bounds_pass.ln` | pass | — | — |
| `tests/safety/bounds/range_assign_check_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/range_if.ln` | other | — | no-suffix |
| `tests/safety/bounds/range_if_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/range_linear.ln` | other | — | no-suffix |
| `tests/safety/bounds/range_loop_unsound_fail.ln` | fail | E012 | — |
| `tests/safety/bounds/range_relational.ln` | other | — | no-suffix |
| `tests/safety/bounds/return_constraints.ln` | other | — | no-suffix |
| `tests/safety/bounds/return_constraints_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/bounds/sub_slice_empty_pass.ln` | pass | — | — |
| `tests/safety/bounds/sub_slice_inverted_fail.ln` | fail | E087 | — |
| `tests/safety/bounds/sub_slice_valid_pass.ln` | pass | — | — |
| `tests/safety/bounds/vra_body_return_inference_pass.ln` | pass | — | — |
| `tests/safety/invariant/struct_in_basic_pass.ln` | pass | — | — |
| `tests/safety/invariant/struct_in_interprocedural_pass.ln` | pass | — | — |
| `tests/safety/invariant/struct_in_mutation_fail.ln` | fail | E121 | — |
| `tests/safety/invariant/struct_in_mutation_pass.ln` | pass | — | — |
| `tests/safety/overflow/callarg_overflow_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/cast_to_narrow_pass.ln` | pass | — | — |
| `tests/safety/overflow/char_overflow_add_fail.ln` | fail | E086 | char-prefix |
| `tests/safety/overflow/char_overflow_cond_fail.ln` | fail | E086 | char-prefix |
| `tests/safety/overflow/char_overflow_escapes_pass.ln` | pass | — | char-prefix |
| `tests/safety/overflow/char_overflow_mul_fail.ln` | fail | E086 | char-prefix |
| `tests/safety/overflow/char_u16_mul_ub_fail.ln` | fail | E086 | char-prefix |
| `tests/safety/overflow/field_assign_overflow_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/i8_neg_overflow_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/mixed_signed_pass.ln` | pass | — | — |
| `tests/safety/overflow/return_overflow_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/struct_init_overflow_fail.ln` | fail | E012 | — |
| `tests/safety/overflow/u16_to_u8_assign_pass.ln` | pass | — | — |
| `tests/safety/overflow/u32_mul_wrap_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/u8_add_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/u8_mul_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/u8_saturating_pass.ln` | pass | — | — |
| `tests/safety/overflow/u8_sub_fail.ln` | fail | E086 | — |
| `tests/safety/overflow/u8_wrap_pass.ln` | pass | — | — |
| `tests/safety/overflow/unsafe_bypasses_pass.ln` | pass | — | — |
| `tests/safety/overflow/vra_const_folding_pass.ln` | pass | — | — |
| `tests/safety/overflow/vra_return_range_pass.ln` | pass | — | — |
| `tests/safety/overflow/widening_pass.ln` | pass | — | — |
| `tests/safety/refinement/composed_constraints_pass.ln` | pass | — | — |
| `tests/safety/refinement/equal_constraint_fail.ln` | fail | E086 | — |
| `tests/safety/refinement/equal_constraint_pass.ln` | pass | — | — |
| `tests/safety/refinement/exclusive_lt_fail.ln` | fail | E086 | — |
| `tests/safety/refinement/lower_bound_fail.ln` | fail | E086 | — |
| `tests/safety/refinement/nonzero_pass.ln` | pass | — | — |
| `tests/safety/refinement/refinement_runtime_alias_pass.ln` | pass | — | — |
| `tests/safety/refinement/upper_bound_fail.ln` | fail | E086 | — |
| `tests/safety/refinement/within_bound_pass.ln` | pass | — | — |
| `tests/safety/vra/and_chain_in_guard_pass.ln` | pass | — | — |
| `tests/safety/vra/array_index_safe_pass.ln` | pass | — | — |
| `tests/safety/vra/constraint_param_pass.ln` | pass | — | — |
| `tests/safety/vra/if_narrows_then_branch_pass.ln` | pass | — | — |
| `tests/safety/vra/in_guard_narrows_pass.ln` | pass | — | — |
| `tests/safety/vra/literal_overflow_fail.ln` | fail | E086 | — |
| `tests/safety/vra/literal_widening_pass.ln` | pass | — | — |
| `tests/safety/vra/return_constrained_to_unsigned_pass.ln` | pass | — | — |
| `tests/safety/vra/return_negative_to_unsigned_fail.ln` | fail | E086 | — |
| `tests/safety/vra/while_decreasing_pass.ln` | pass | — | — |
| `tests/safety/vra/while_no_measure_in_func_fail.ln` | fail | — | no-EXPECT |

### 14 generics (13)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/comptime/comptime_call_proc_fail.ln` | fail | E101 | — |
| `tests/comptime_if.ln` | other | — | no-suffix |
| `tests/generics/generic_combinator_pass.ln` | pass | — | — |
| `tests/generics/generic_enum_option_pass.ln` | pass | — | — |
| `tests/generics/generic_enum_result_pass.ln` | pass | — | — |
| `tests/generics/generic_func_explicit_pass.ln` | pass | — | — |
| `tests/generics/generic_func_infer_pass.ln` | pass | — | — |
| `tests/generics/generic_nested_pass.ln` | pass | — | — |
| `tests/generics/generic_option_map_pass.ln` | pass | — | — |
| `tests/generics/generic_std_option_pass.ln` | pass | — | — |
| `tests/generics/generic_std_result_pass.ln` | pass | — | — |
| `tests/generics/generic_struct_infer_pass.ln` | pass | — | — |
| `tests/generics/generic_struct_pass.ln` | pass | — | — |

### 15 match (29)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_dependent_size_mismatch_fail.ln` | fail | E087 | char-prefix;grab-bag |
| `tests/core/match_advanced.ln` | other | — | no-suffix |
| `tests/niche/bool_payload_pass.ln` | pass | — | — |
| `tests/niche/case_pointer_match_pass.ln` | pass | — | — |
| `tests/niche/generic_pointer_niche_pass.ln` | pass | — | — |
| `tests/niche/multi_empty_pointer_pass.ln` | pass | — | — |
| `tests/niche/multi_payload_runtime_pass.ln` | pass | — | — |
| `tests/niche/multi_payload_tagged_pass.ln` | pass | — | — |
| `tests/niche/nested_cascade_pass.ln` | pass | — | — |
| `tests/niche/nested_runtime_pass.ln` | pass | — | — |
| `tests/niche/option_pointer_pass.ln` | pass | — | — |
| `tests/niche/pointer_array_pass.ln` | pass | — | — |
| `tests/niche/pure_empty_enum_pass.ln` | pass | — | — |
| `tests/niche/refinement_below_only_pass.ln` | pass | — | — |
| `tests/niche/refinement_payload_pass.ln` | pass | — | — |
| `tests/niche/refinement_runtime_pass.ln` | pass | — | — |
| `tests/niche/refinement_split_pass.ln` | pass | — | — |
| `tests/union/optional_construct_pass.ln` | pass | — | — |
| `tests/union/optional_narrow_pass.ln` | pass | — | — |
| `tests/union/optional_use_without_check_fail.ln` | fail | E063 | — |
| `tests/union/union_case_runtime_pass.ln` | pass | — | — |
| `tests/union/union_construct_pass.ln` | pass | — | — |
| `tests/union/union_if_narrow_runtime_pass.ln` | pass | — | — |
| `tests/union/union_multi_marker_if_fail.ln` | fail | E063 | — |
| `tests/union/union_no_niche_fail.ln` | fail | E064 | — |
| `tests/union/union_optionality_unify_pass.ln` | pass | — | — |
| `tests/union/union_passthrough_pass.ln` | pass | — | — |
| `tests/union/union_pointer_layout_pass.ln` | pass | — | — |
| `tests/union/union_struct_field_pass.ln` | pass | — | — |

### 16 modules (2)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/modules/module_alias_pass.ln` | pass | — | — |
| `tests/modules/module_qualified_access_pass.ln` | pass | — | — |

### 18 unsafe (3)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/safety/unsafe_deref_fail.ln` | fail | — | no-EXPECT |
| `tests/safety/unsafe_nested.ln` | other | — | no-suffix |
| `tests/safety/unsafe_valid.ln` | other | — | no-suffix |

### 19 memory (21)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_outparam_uninit_fail.ln` | fail | E005 | char-prefix;grab-bag |
| `tests/characterization/char_uninit_partial_assign_fail.ln` | fail | E005 | char-prefix;grab-bag |
| `tests/characterization/char_uninit_scalar_read_fail.ln` | fail | E005 | char-prefix;grab-bag |
| `tests/core/uninit_fail.ln` | fail | E005 | — |
| `tests/init/array_comprehension_expr_position_fail.ln` | fail | E100 | — |
| `tests/init/array_comprehension_pass.ln` | pass | — | — |
| `tests/init/array_literal_pass.ln` | pass | — | — |
| `tests/init/array_ref_of_uninit_pass.ln` | pass | — | — |
| `tests/init/array_uninit_element_read_fail.ln` | fail | E005 | — |
| `tests/init/field_branch_merge_pass.ln` | pass | — | — |
| `tests/init/field_construct_pass.ln` | pass | — | — |
| `tests/init/field_nested_branch_merge_pass.ln` | pass | — | — |
| `tests/init/field_nested_construct_pass.ln` | pass | — | — |
| `tests/init/field_nested_uninit_fail.ln` | fail | E005 | — |
| `tests/init/field_nested_whole_fail.ln` | fail | E019 | — |
| `tests/init/field_partial_use_fail.ln` | fail | E019 | — |
| `tests/init/field_uninit_read_fail.ln` | fail | E005 | — |
| `tests/uninit_fail.ln` | fail | E005 | — |
| `tests/uninit_if_fail.ln` | fail | E005 | — |
| `tests/uninit_if_pass.ln` | pass | — | — |
| `tests/uninit_pass.ln` | pass | — | — |

### 20 stdlib (12)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/stdlib/c_include_test.ln` | other | — | no-suffix |
| `tests/stdlib/dummy.ln` | other | — | no-suffix |
| `tests/stdlib/dummy_private.ln` | other | — | no-suffix |
| `tests/stdlib/import_test.ln` | other | — | no-suffix |
| `tests/stdlib/math_smoke_pass.ln` | pass | — | — |
| `tests/stdlib/private_cross_fail.ln` | fail | E084 | — |
| `tests/stdlib/private_pub_pass.ln` | pass | — | — |
| `tests/stdlib/test_bump.ln` | other | — | no-suffix |
| `tests/stdlib/test_extern.ln` | other | — | no-suffix |
| `tests/stdlib/test_fs.ln` | other | — | no-suffix |
| `tests/stdlib/test_io.ln` | other | — | no-suffix |
| `tests/stdlib/test_mem.ln` | other | — | no-suffix |

### IMPL codegen (7)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/codegen/const_attr_pointer_reader_pass.ln` | pass | — | — |
| `tests/codegen/struct_array_field_index_pass.ln` | pass | — | — |
| `tests/emit/case_expression_payload_pass.ln` | pass | — | — |
| `tests/emit/ownership_abi.ln` | other | — | no-suffix |
| `tests/emit/sized_slice_abi.ln` | other | — | no-suffix |
| `tests/emit/str_eq_literal_pass.ln` | pass | — | — |
| `tests/emit/var_prim_abi.ln` | other | — | no-suffix |

### INTEG examples (18)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/examples/fixed_array_arg_pass.ln` | pass | — | — |
| `tests/examples/kw_match_pass.ln` | pass | — | — |
| `tests/examples/refine_ident_bound_pass.ln` | pass | — | — |
| `tests/examples/simd_lexer_pass.ln` | pass | — | — |
| `tests/examples/simd_load_checked_pass.ln` | pass | — | — |
| `tests/examples/simd_load_oob_fail.ln` | fail | E085 | — |
| `tests/examples/simd_load_pass.ln` | pass | — | — |
| `tests/examples/simd_mask_pass.ln` | pass | — | — |
| `tests/examples/simd_scan_proven_pass.ln` | pass | — | — |
| `tests/examples/simd_splat_store_pass.ln` | pass | — | — |
| `tests/examples/simd_vec_lane_oob_fail.ln` | fail | — | no-EXPECT |
| `tests/examples/simd_vec_pass.ln` | pass | — | — |
| `tests/examples/simd_vra_count_pass.ln` | pass | — | — |
| `tests/examples/soa_output_pass.ln` | pass | — | — |
| `tests/examples/vra_and_flow_peek_pass.ln` | pass | — | — |
| `tests/examples/vra_struct_field_slice_pass.ln` | pass | — | — |
| `tests/examples/vra_while_lt_scalar_pass.ln` | pass | — | — |
| `tests/examples/vra_while_lt_wide_load_pass.ln` | pass | — | — |

### ?? (unmapped) (29)

| current path | kind | EXPECT | flags |
|:--|:--|:--|:--|
| `tests/characterization/char_and_flow_leak_fail.ln` | fail | E085 | char-prefix;grab-bag |
| `tests/characterization/char_duplicate_param_fail.ln` | fail | E013 | char-prefix;grab-bag |
| `tests/characterization/char_free_size_var_fail.ln` | fail | E100 | char-prefix;grab-bag |
| `tests/characterization/char_len_param_callsite_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_mutable_global_fail.ln` | fail | E100 | char-prefix;grab-bag |
| `tests/characterization/char_null_compare_pass.ln` | pass | — | char-prefix;grab-bag |
| `tests/characterization/char_shift_out_of_range_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_sign_change_implicit_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_signed_narrow_implicit_fail.ln` | fail | E086 | char-prefix;grab-bag |
| `tests/characterization/char_str_plus_str_fail.ln` | fail | E012 | char-prefix;grab-bag |
| `tests/characterization/char_toplevel_parse_error_fail.ln` | fail | E100 | char-prefix;grab-bag |
| `tests/characterization/char_var_mandatory_fail.ln` | fail | E017 | char-prefix;grab-bag |
| `tests/core/const_global_pass.ln` | pass | — | — |
| `tests/core/fun_keyword_fail.ln` | fail | E100 | — |
| `tests/core/immutable_assign_fail.ln` | fail | E009 | — |
| `tests/core/immutable_decl_pass.ln` | pass | — | — |
| `tests/core/immutable_var_coexist_pass.ln` | pass | — | — |
| `tests/core/implicit_decl_fail.ln` | fail | E009 | — |
| `tests/core/implicit_decl_pass.ln` | pass | — | — |
| `tests/core/math.ln` | other | — | no-suffix |
| `tests/core/math_test.ln` | other | — | no-suffix |
| `tests/core/panic_basic_pass.ln` | pass | — | — |
| `tests/core/shadowing_fail.ln` | fail | E013 | — |
| `tests/core/shift_operators.ln` | other | — | no-suffix |
| `tests/core/string_escape.ln` | other | — | no-suffix |
| `tests/core/termination_fail.ln` | fail | — | no-EXPECT |
| `tests/core/termination_pass.ln` | pass | — | — |
| `tests/core/ufcs_test.ln` | other | — | no-suffix |
| `tests/core/undefined_pass.ln` | pass | — | — |
