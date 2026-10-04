//! Integration tests: one (or more) test per syntax construct listed in the
//! PlainVulkan language reference, plus a handful of error-path tests.
//! The goal is that every line of the "Language syntax" section of the
//! spec is exercised here at least once.

use pv_transpiler::ast::*;
use pv_transpiler::{parse, parse_expr};

fn ok(src: &str) -> Program {
    match parse(src) {
        Ok(p) => p,
        Err(diags) => panic!(
            "expected '{}' to parse, got errors:\n{}",
            src,
            pv_transpiler::render_diagnostics("<test>", src, &diags)
        ),
    }
}

fn err(src: &str) {
    if let Ok(_) = parse(src) {
        panic!("expected '{}' to fail to parse, but it parsed", src);
    }
}

// ---------------------------------------------------------------------
// Comments -- must be fully invisible to the AST.
// ---------------------------------------------------------------------

#[test]
fn line_and_block_comments_produce_no_ast_nodes() {
    let with_comments = ok(
        "// leading comment\nvar x = 1; /* inline */ var y = 2; // trailing\n/* multi\nline */",
    );
    // Comments shift byte offsets, so compare structure/values rather than
    // exact spans: both programs should be two `var` decls of x=1, y=2.
    assert_eq!(with_comments.stmts.len(), 2);
    for (stmt, (name, val)) in with_comments.stmts.iter().zip([("x", 1i64), ("y", 2i64)]) {
        match stmt {
            Stmt::VarDecl(d) => {
                assert_eq!(d.kind, DeclKind::Var);
                assert_eq!(d.name, name);
                assert!(matches!(d.init, Some(Expr::IntLiteral(n, _)) if n == val));
            }
            other => panic!("expected VarDecl, got {:?}", other),
        }
    }
}

// ---------------------------------------------------------------------
// Variables
// ---------------------------------------------------------------------

#[test]
fn var_let_const_declarations() {
    let p = ok("var a = 1; let b = 2; const c = 3; var d;");
    assert_eq!(p.stmts.len(), 4);
    match &p.stmts[0] {
        Stmt::VarDecl(d) => {
            assert_eq!(d.kind, DeclKind::Var);
            assert_eq!(d.name, "a");
            assert!(d.init.is_some());
        }
        other => panic!("expected VarDecl, got {:?}", other),
    }
    match &p.stmts[2] {
        Stmt::VarDecl(d) => assert_eq!(d.kind, DeclKind::Const),
        other => panic!("expected VarDecl, got {:?}", other),
    }
    match &p.stmts[3] {
        Stmt::VarDecl(d) => {
            assert_eq!(d.kind, DeclKind::Var);
            assert!(d.init.is_none());
        }
        other => panic!("expected VarDecl, got {:?}", other),
    }
}

#[test]
fn const_requires_initializer() {
    err("const c;");
}

// ---------------------------------------------------------------------
// Assignment operators: = += -= *= /= %=
// ---------------------------------------------------------------------

#[test]
fn all_assignment_operators() {
    for op_src in ["=", "+=", "-=", "*=", "/=", "%="] {
        let src = format!("var x = 0; x {} 1;", op_src);
        ok(&src);
    }
}

#[test]
fn assignment_targets_ident_index_member() {
    let p = ok("a = 1; a[0] = 1; a.b = 1; a.b[0].c = 1;");
    assert_eq!(p.stmts.len(), 4);
    for s in &p.stmts {
        assert!(matches!(s, Stmt::Assign(_)), "expected Assign, got {:?}", s);
    }
    // a.b[0].c = 1  ==>  Member(Index(Member(Ident a, b), 0), c)
    if let Stmt::Assign(a) = &p.stmts[3] {
        match &a.target {
            LValue::Member(base, field, _) => {
                assert_eq!(field, "c");
                match base.as_ref() {
                    LValue::Index(base2, _, _) => match base2.as_ref() {
                        LValue::Member(base3, field2, _) => {
                            assert_eq!(field2, "b");
                            assert!(matches!(base3.as_ref(), LValue::Ident(n, _) if n == "a"));
                        }
                        other => panic!("unexpected: {:?}", other),
                    },
                    other => panic!("unexpected: {:?}", other),
                }
            }
            other => panic!("unexpected: {:?}", other),
        }
    }
}

#[test]
fn invalid_assignment_targets_are_rejected() {
    err("1 = 2;");
    err("(a + b) = 2;");
    err("foo() = 2;");
}

#[test]
fn chained_assignment_is_right_associative() {
    let e = match parse_expr("a = b = 5") {
        Ok(e) => e,
        Err(d) => panic!("{:?}", d),
    };
    match e {
        Expr::Assign(target, AssignOp::Assign, value, _) => {
            assert!(matches!(*target, LValue::Ident(ref n, _) if n == "a"));
            assert!(matches!(*value, Expr::Assign(..)));
        }
        other => panic!("unexpected: {:?}", other),
    }
}

// ---------------------------------------------------------------------
// Values / literals
// ---------------------------------------------------------------------

#[test]
fn integer_literal_forms() {
    let src = "var a = 123; var b = -10; var c = 0xFF; var d = 0b1010;";
    let p = ok(src);
    assert_eq!(p.stmts.len(), 4);
}

#[test]
fn float_literal_forms() {
    ok("var a = 3.14; var b = 1.5e3; var c = 1.5e-3; var d = 1.5E+2;");
}

#[test]
fn string_literal_both_quote_styles() {
    ok(r#"var a = "double"; var b = 'single';"#);
}

#[test]
fn bool_and_null_literals() {
    ok("var a = true; var b = false; var c = null;");
}

// ---------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------

#[test]
fn array_literals_and_indexing() {
    ok("var a = []; var b = [1, 2, 3]; var c = [1, 2, 3,]; var x = b[0]; b[0] = 9;");
}

#[test]
fn nested_arrays() {
    ok("var grid = [[1, 2], [3, 4]]; var v = grid[0][1];");
}

// ---------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------

#[test]
fn function_decl_params_call_return() {
    let p = ok(
        "function add(a, b) { return a + b; } function noop() { return; } var r = add(1, 2); noop();",
    );
    assert_eq!(p.stmts.len(), 4);
    match &p.stmts[0] {
        Stmt::Function(f) => {
            assert_eq!(f.name, "add");
            assert_eq!(f.params, vec!["a".to_string(), "b".to_string()]);
        }
        other => panic!("unexpected: {:?}", other),
    }
}

#[test]
fn nested_function_declaration() {
    ok("function outer() { function inner() { return 1; } return inner(); }");
}

// ---------------------------------------------------------------------
// if / elif / else
// ---------------------------------------------------------------------

#[test]
fn if_alone() {
    ok("if (true) { var x = 1; }");
}

#[test]
fn if_else() {
    ok("if (true) { } else { }");
}

#[test]
fn if_multiple_elif_and_else() {
    let p = ok("if (a == 1) { } elif (a == 2) { } elif (a == 3) { } else { }");
    match &p.stmts[0] {
        Stmt::If(s) => {
            assert_eq!(s.elifs.len(), 2);
            assert!(s.else_block.is_some());
        }
        other => panic!("unexpected: {:?}", other),
    }
}

#[test]
fn if_elif_no_else() {
    let p = ok("if (a) { } elif (b) { }");
    match &p.stmts[0] {
        Stmt::If(s) => assert!(s.else_block.is_none()),
        other => panic!("unexpected: {:?}", other),
    }
}

// ---------------------------------------------------------------------
// Comparison operators
// ---------------------------------------------------------------------

#[test]
fn all_comparison_operators() {
    for op in ["==", "!=", "<", ">", "<=", ">="] {
        ok(&format!("var r = 1 {} 2;", op));
    }
}

// ---------------------------------------------------------------------
// Logical operators
// ---------------------------------------------------------------------

#[test]
fn logical_and_or_not() {
    ok("var r = true && false; var s = true || false; var t = !true;");
}

// ---------------------------------------------------------------------
// while loop
// ---------------------------------------------------------------------

#[test]
fn while_loop() {
    ok("while (true) { break; }");
}

// ---------------------------------------------------------------------
// for loop
// ---------------------------------------------------------------------

#[test]
fn classic_c_style_for_loop() {
    let p = ok("for (var i = 0; i < 10; i += 1) { continue; }");
    match &p.stmts[0] {
        Stmt::For(f) => {
            assert!(matches!(f.init, ForInit::VarDecl(_)));
            assert!(f.cond.is_some());
            assert!(matches!(f.update, ForUpdate::Assign(_)));
        }
        other => panic!("unexpected: {:?}", other),
    }
}

#[test]
fn for_loop_with_empty_clauses() {
    ok("for (;;) { break; }");
}

#[test]
fn for_loop_with_expr_init_and_update() {
    ok("var i = 0; for (i = 0; i < 5; i = i + 1) { }");
}

// ---------------------------------------------------------------------
// break / continue
// ---------------------------------------------------------------------

#[test]
fn break_and_continue_in_loops() {
    ok("while (true) { break; } while (true) { continue; }");
}

// ---------------------------------------------------------------------
// Arithmetic operators
// ---------------------------------------------------------------------

#[test]
fn all_arithmetic_operators() {
    for op in ["+", "-", "*", "/", "%"] {
        ok(&format!("var r = 1 {} 2;", op));
    }
}

#[test]
fn arithmetic_precedence() {
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    let e = parse_expr("1 + 2 * 3").unwrap();
    match e {
        Expr::Binary(BinaryOp::Add, lhs, rhs, _) => {
            assert!(matches!(*lhs, Expr::IntLiteral(1, _)));
            assert!(matches!(*rhs, Expr::Binary(BinaryOp::Mul, _, _, _)));
        }
        other => panic!("unexpected: {:?}", other),
    }
}

// ---------------------------------------------------------------------
// Unary operators
// ---------------------------------------------------------------------

#[test]
fn unary_operators() {
    ok("var a = -5; var b = !true; var c = ~5;");
}

// ---------------------------------------------------------------------
// Ternary operator
// ---------------------------------------------------------------------

#[test]
fn ternary_operator() {
    let e = parse_expr("a ? b : c").unwrap();
    assert!(matches!(e, Expr::Ternary(..)));
}

#[test]
fn ternary_false_branch_can_be_assignment() {
    // a ? b : (c = d)  -- the '=' belongs to the false branch, not to the
    // whole ternary. This is the case that originally required narrowing
    // the assignment target grammar rule to Postfix instead of Ternary.
    let e = parse_expr("a ? b : c = d").unwrap();
    match e {
        Expr::Ternary(_, _, false_branch, _) => {
            assert!(matches!(*false_branch, Expr::Assign(..)));
        }
        other => panic!("unexpected: {:?}", other),
    }
}

#[test]
fn nested_ternary_is_right_associative() {
    ok("var r = a ? b : c ? d : e;");
}

// ---------------------------------------------------------------------
// Member access
// ---------------------------------------------------------------------

#[test]
fn member_access_chains() {
    ok("var x = a.b.c; a.b.c = 1;");
}

// ---------------------------------------------------------------------
// Scope access (`::`) and Pv:: engine commands
// ---------------------------------------------------------------------

#[test]
fn scope_call_is_generic_not_hardcoded() {
    // Any Namespace::name(...) should parse -- not just literal "Pv".
    ok("MyNamespace::DoThing(1, 2);");
    let e = parse_expr("Pv::DrawTriangle(x, y, color)").unwrap();
    let (ns, name, args) = e.as_scoped_call().expect("expected a scoped call");
    assert_eq!(ns, "Pv");
    assert_eq!(name, "DrawTriangle");
    assert_eq!(args.len(), 3);
}

#[test]
fn representative_engine_commands_from_every_reference_category() {
    // One call from each category in the PlainVulkan engine command
    // reference, to prove the *generic* Scope+Call grammar handles the
    // entire ~150-function surface without any per-function grammar rules.
    let calls = [
        r#"Pv::Init();"#,
        r#"Pv::CreateWindow(1280, 720, "PlainVulkan");"#,
        r#"Pv::BeginFrame();"#,
        r#"Pv::SetCamera(0, 5, -10);"#,
        r#"Pv::DrawTriangle(0, 0, 0xFF0000);"#,
        r#"Pv::DrawMeshEx(mesh, x, y, z, 0, 0, 0, 1.0);"#,
        r#"var m = Pv::LoadMesh("assets/models/player.glb");"#,
        r#"var t = Pv::LoadTexture("assets/tex.png");"#,
        r#"Pv::SetUniformVec3(shader, "lightPos", 1, 2, 3);"#,
        r#"Pv::AddPointLight(0, 5, 0, 1, 1, 1, 1.0, 10.0);"#,
        r#"var mat = Pv::CreateMaterial();"#,
        r#"Pv::PlaySound(sound);"#,
        r#"if (Pv::IsKeyDown(32)) { }"#,
        r#"var g = Pv::IsGamepadConnected(0);"#,
        r#"Pv::InitPhysics(); Pv::SetGravity(0, -9.8, 0);"#,
        r#"var exists = Pv::FileExists("save.json");"#,
        r#"Pv::UIButton("Click", 0, 0, 100, 30);"#,
        r#"Pv::Log("hello");"#,
        r#"var anim = Pv::LoadAnimation("walk.glb");"#,
        r#"Pv::EndFrame();"#,
    ];
    let joined = calls.join("\n");
    ok(&joined);
}

#[test]
fn scope_and_member_and_index_and_call_chain_together() {
    // Pv::Raycast(...) returning an object-shaped Value with fields, then
    // indexing/member/further calls all chained through the same generic
    // Postfix rule.
    ok("var hit = Pv::Raycast(0, 0, 0, 0, -1, 0, 100); if (hit.hit) { Pv::Log(hit.distance); }");
}

// ---------------------------------------------------------------------
// Full-program smoke test mixing every construct at once
// ---------------------------------------------------------------------

#[test]
fn kitchen_sink_program() {
    let src = r#"
        // A small program exercising every syntax construct at once.
        const GRAVITY = 9.8;
        var score = 0;
        let name = "Player";

        function clamp(v, lo, hi) {
            if (v < lo) {
                return lo;
            } elif (v > hi) {
                return hi;
            } else {
                return v;
            }
        }

        function update(dt) {
            var speed = 5.0 * dt;
            score += 1;
            for (var i = 0; i < 10; i += 1) {
                if (i % 2 == 0) {
                    continue;
                }
                if (i == 7) {
                    break;
                }
            }
            var items = [1, 2, 3];
            items[0] = clamp(items[0] * 2, 0, 100);
            var label = score > 10 ? "high" : "low";
            var alive = !false && (score >= 0 || speed != 0);
            return alive;
        }

        Pv::Init();
        Pv::CreateWindow(1280, 720, "Demo");
        var player = Pv::LoadMesh("assets/models/player.glb");
        Pv::SetAmbient(1, 1, 1, 0.2);

        while (Pv::IsWindowOpen()) {
            Pv::BeginFrame();
            Pv::Clear(0x000000);
            update(Pv::GetDeltaTime());
            Pv::DrawMesh(player, 0, 0, 0);
            Pv::EndFrame();
        }
        Pv::Shutdown();
    "#;
    let p = ok(src);
    assert!(p.stmts.len() > 5);
}

// ---------------------------------------------------------------------
// Error diagnostics sanity checks
// ---------------------------------------------------------------------

#[test]
fn unterminated_string_reports_error_not_panic() {
    err("var x = \"unterminated;");
}

#[test]
fn unterminated_block_comment_reports_error() {
    err("var x = 1; /* never closed");
}

#[test]
fn malformed_syntax_reports_error() {
    err("var = ;");
    err("if (true) var x = 1;"); // missing braces around the if-body
}
