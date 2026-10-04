//! `pvc` -- the PlainVulkan syntax checker / AST inspector.
//! This is the binary `pv check <file>` shells out to.

use std::fs;
use std::process::ExitCode;

fn usage() -> ! {
    eprintln!("Usage:");
    eprintln!("  pvc check <file.pv>        Parse a file and report syntax errors");
    eprintln!("  pvc ast <file.pv>          Parse a file and print its AST as JSON");
    std::process::exit(2);
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        usage();
    }

    let cmd = args[1].as_str();
    let path = &args[2];

    let src = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: could not read '{}': {}", path, e);
            return ExitCode::FAILURE;
        }
    };

    match cmd {
        "check" => match pv_transpiler::parse(&src) {
            Ok(program) => {
                let fn_count = program
                    .stmts
                    .iter()
                    .filter(|s| matches!(s, pv_transpiler::ast::Stmt::Function(_)))
                    .count();
                let stmt_count = program.stmts.len() - fn_count;
                println!(
                    "OK: {} parses cleanly ({} top-level function(s), {} other top-level statement(s))",
                    path, fn_count, stmt_count
                );
                ExitCode::SUCCESS
            }
            Err(diags) => {
                eprint!("{}", pv_transpiler::render_diagnostics(path, &src, &diags));
                eprintln!("{}: {} error(s)", path, diags.len());
                ExitCode::FAILURE
            }
        },
        "ast" => match pv_transpiler::parse(&src) {
            Ok(program) => {
                match serde_json::to_string_pretty(&program) {
                    Ok(json) => {
                        println!("{}", json);
                        ExitCode::SUCCESS
                    }
                    Err(e) => {
                        eprintln!("error: failed to serialize AST: {}", e);
                        ExitCode::FAILURE
                    }
                }
            }
            Err(diags) => {
                eprint!("{}", pv_transpiler::render_diagnostics(path, &src, &diags));
                eprintln!("{}: {} error(s)", path, diags.len());
                ExitCode::FAILURE
            }
        },
        _ => usage(),
    }
}
