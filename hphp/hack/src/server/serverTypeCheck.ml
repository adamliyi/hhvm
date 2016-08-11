(**
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 *)

open Core
open ServerCheckUtils
open ServerEnv
open Reordered_argument_collections
open Utils

module SLC = ServerLocalConfig

(*****************************************************************************)
(* Debugging *)
(*****************************************************************************)

let print_defs prefix defs =
  List.iter defs begin fun (_, fname) ->
    Printf.printf "  %s %s\n" prefix fname;
  end

let print_fast_pos fast_pos =
  SMap.iter fast_pos begin fun x (funs, classes) ->
    Printf.printf "File: %s\n" x;
    print_defs "Fun" funs;
    print_defs "Class" classes;
  end;
  Printf.printf "\n";
  flush stdout;
  ()

let print_fast fast =
  SMap.iter fast begin fun x (funs, classes) ->
    Printf.printf "File: %s\n" x;
    SSet.iter funs (Printf.printf "  Fun %s\n");
    SSet.iter classes (Printf.printf "  Class %s\n");
  end;
  Printf.printf "\n";
  flush stdout;
  ()

let debug_print_fast_keys genv name fast =
  ServerDebug.log genv begin fun () ->
    let open Hh_json in
    let files = Relative_path.Map.fold fast ~init:[] ~f:begin fun k _v acc ->
      JSON_String (Relative_path.suffix k) :: acc
    end in
    let decls = Relative_path.Map.fold fast ~init:[] ~f:begin fun _k v acc ->
      let {FileInfo.n_funs; n_classes; n_types; n_consts} = v in
      let prepend_json_strings decls acc =
        SSet.fold decls ~init:acc ~f:(fun n acc -> JSON_String n :: acc) in
      let acc = prepend_json_strings n_funs acc in
      let acc = prepend_json_strings n_classes acc in
      let acc = prepend_json_strings n_types acc in
      let acc = prepend_json_strings n_consts acc in
      acc
    end in
    JSON_Object [
      "type", JSON_String "incremental_files";
      "name", JSON_String name;
      "files", JSON_Array files;
      "decls", JSON_Array decls;
    ]
  end

(*****************************************************************************)
(* Given a set of Ast.id list produce a SSet.t (got rid of the positions)    *)
(*****************************************************************************)

let set_of_idl l =
  List.fold_left l ~f:(fun acc (_, x) -> SSet.add acc x) ~init:SSet.empty

(*****************************************************************************)
(* We want add all the declarations that were present in a file *before* the
 * current modification. The scenario:
 * File foo.php was defining the class A.
 * The user gets rid of class A (in foo.php)
 * In general, the type-checker determines what must be re-declared or
 * re-typechecked, by comparing the old and the new type-definitions.
 * That's why we are adding the 'old' definitions to the file.
 * In this case, the redecl phase (typing/typing_redecl_service.ml) is going
 * to compare the 'old' definition of A with the new one. It will realize that
 * the new one is missing, and go ahead and retype everything that depends
 * on A.
 * Without a call to add_old_decls, the class A wouldn't appear anywhere,
 * and we wouldn't realize that we have to re-check the types that depend
 * on A.
 *)
(*****************************************************************************)

let add_old_decls old_files_info fast =
  Relative_path.Map.fold fast ~f:begin fun filename info_names acc ->
    match Relative_path.Map.get old_files_info filename with
    | Some {FileInfo.consider_names_just_for_autoload = true; _}
    | None -> acc
    | Some old_info ->
      let old_info_names = FileInfo.simplify old_info in
      let info_names = FileInfo.merge_names old_info_names info_names in
      Relative_path.Map.add acc ~key:filename ~data:info_names
  end ~init:fast

let reparse_infos files_info fast =
  Relative_path.Map.fold fast ~f:begin fun x _y acc ->
    try
      let info = Relative_path.Map.find_unsafe x files_info in
      if info.FileInfo.consider_names_just_for_autoload then acc else
      Relative_path.Map.add acc ~key:x ~data:info
    with Not_found -> acc
  end ~init:Relative_path.Map.empty

(*****************************************************************************)
(* Removes the names that were defined in the files *)
(*****************************************************************************)

let remove_decls env fast_parsed =
  Relative_path.Map.iter fast_parsed begin fun fn _ ->
    match Relative_path.Map.get env.files_info fn with
    | Some {FileInfo.consider_names_just_for_autoload = true; _}
    | None -> ()
    | Some {FileInfo.
             funs = funl;
             classes = classel;
             typedefs = typel;
             consts = constl;
             file_mode = _;
             comments = _;
             consider_names_just_for_autoload = _} ->
      let funs = set_of_idl funl in
      let classes = set_of_idl classel in
      let typedefs = set_of_idl typel in
      let consts = set_of_idl constl in
      NamingGlobal.remove_decls ~funs ~classes ~typedefs ~consts
  end;
  env

(*****************************************************************************)
(* Removes the files that failed *)
(*****************************************************************************)

let remove_failed fast failed =
  Relative_path.Set.fold failed ~init:fast ~f:Relative_path.Map.remove

(*****************************************************************************)
(* Parses the set of modified files *)
(*****************************************************************************)

let parsing genv env =
  let files_map = SSet.fold env.files_to_check ~init:SMap.empty
    ~f:(fun path map ->
      let content = File_content.get_content @@ SMap.find_unsafe path
        env.edited_files in
      SMap.add map path content) in
  let to_check = SSet.fold env.files_to_check ~init:env.failed_parsing
    ~f:(fun path set ->
      let fn = Relative_path.create Relative_path.Root path in
      Relative_path.Set.add set fn) in
  Parser_heap.ParserHeap.remove_batch env.failed_parsing;
  Fixmes.HH_FIXMES.remove_batch to_check;
  HackSearchService.MasterApi.clear_shared_memory to_check;
  SharedMem.collect `gentle;
  let get_next = MultiWorker.next
   genv.workers (Relative_path.Set.elements env.failed_parsing) in
  Parsing_service.go genv.workers files_map ~get_next

(*****************************************************************************)
(* At any given point in time, we want to know what each file defines.
 * The datastructure that maintains this information is called file_info.
 * This code updates the file information.
 *)
(*****************************************************************************)

let update_file_info env fast_parsed =
  Typing_deps.update_files fast_parsed;
  let files_info = Relative_path.Map.union fast_parsed env.files_info in
  files_info

(*****************************************************************************)
(* Defining the global naming environment.
 * Defines an environment with the names of all the globals (classes/funs).
 *)
(*****************************************************************************)

let declare_names env fast_parsed =
  let env = remove_decls env fast_parsed in
  let errorl, failed_naming =
    Relative_path.Map.fold fast_parsed ~f:begin fun k v (errorl, failed) ->
      let errorl', failed'= NamingGlobal.ndecl_file k v in
      let errorl = Errors.merge errorl' errorl in
      let failed = Relative_path.Set.union failed' failed in
      errorl, failed
    end ~init:(Errors.empty, Relative_path.Set.empty) in
  let fast = remove_failed fast_parsed failed_naming in
  let fast = FileInfo.simplify_fast fast in
  env, errorl, failed_naming, fast

(*****************************************************************************)
(* Function called after parsing, does nothing by default. *)
(*****************************************************************************)

let hook_after_parsing = ref None

(*****************************************************************************)
(* Where the action is! *)
(*****************************************************************************)

let type_check genv env =

  (* PREPARE FOR PARSING *)
  let failed_parsing_ide, failed_parsing_ = Relative_path.Set.partition
    (fun fn -> let path = Relative_path.to_absolute fn in
      SMap.exists (fun p _ -> p = path) env.edited_files) env.failed_parsing in
  let files_to_check_ = Relative_path.Set.fold failed_parsing_ide
    ~init:SSet.empty ~f:(fun fn set ->
      SSet.add set (Relative_path.to_absolute fn)) in
  let check_now = SSet.filter files_to_check_ (fun s -> not @@
      File_content.being_edited @@ SMap.find_unsafe s env.edited_files) in
  let env = {env with failed_parsing = failed_parsing_;
    files_to_check = check_now} in
  let reparse_count = Relative_path.Set.cardinal env.failed_parsing +
  SSet.cardinal env.files_to_check in
  Printf.eprintf "******************************************\n";
  Hh_logger.log "Files to recompute: %d" reparse_count;

  (* RESET HIGHLIGHTS CACHE FOR RECHECKED IDE FILES *)
  let symbols_cache = SSet.fold env.files_to_check ~init:env.symbols_cache
    ~f:(fun path map -> SMap.remove path map) in

  (* PARSING *)
  let start_t = Unix.gettimeofday () in
  let t = start_t in
  let fast_parsed, errorl, failed_parsing = parsing genv env in
  let hs = SharedMem.heap_size () in
  Hh_logger.log "Heap size: %d" hs;
  HackEventLogger.parsing_end t hs ~parsed_count:reparse_count;
  let t = Hh_logger.log_duration "Parsing" t in

  (* UPDATE FILE INFO *)
  let old_env = env in
  let updates = old_env.failed_parsing in
  let files_info = update_file_info env fast_parsed in
  HackEventLogger.updating_deps_end t;
  let t = Hh_logger.log_duration "Updating deps" t in

  (* BUILDING AUTOLOADMAP *)
  Option.iter !hook_after_parsing begin fun f ->
    f genv old_env { env with files_info } updates
  end;
  HackEventLogger.parsing_hook_end t;
  let t = Hh_logger.log_duration "Parsing Hook" t in

  (* NAMING *)
  let env, errorl', failed_naming, fast =
    declare_names env fast_parsed in

  (* COMPUTES WHAT MUST BE REDECLARED  *)
  let fast = extend_fast fast files_info env.failed_decl in
  let fast = add_old_decls env.files_info fast in
  let errorl = Errors.merge errorl' errorl in

  HackEventLogger.naming_end t;
  let t = Hh_logger.log_duration "Naming" t in

  let bucket_size = genv.local_config.SLC.type_decl_bucket_size in
  debug_print_fast_keys genv "to_redecl_phase1" fast;
  let _, _, to_redecl_phase2, to_recheck1 =
    Decl_redecl_service.redo_type_decl
      ~bucket_size genv.workers env.tcopt fast in
  let to_redecl_phase2 = Typing_deps.get_files to_redecl_phase2 in
  let to_recheck1 = Typing_deps.get_files to_recheck1 in
  let hs = SharedMem.heap_size () in
  Hh_logger.log "Heap size: %d" hs;
  HackEventLogger.first_redecl_end t hs;
  let t = Hh_logger.log_duration "Determining changes" t in

  (* DECLARING TYPES: Phase2 *)
  let fast_redecl_phase2 = extend_fast fast files_info to_redecl_phase2 in
  debug_print_fast_keys genv "to_redecl_phase2" fast_redecl_phase2;
  let errorl', failed_decl, _to_redecl2, to_recheck2 =
    Decl_redecl_service.redo_type_decl
      ~bucket_size genv.workers env.tcopt fast_redecl_phase2 in
  let to_recheck2 = Typing_deps.get_files to_recheck2 in
  let errorl = Errors.merge errorl' errorl in

  (* DECLARING TYPES: merging results of the 2 phases *)
  let fast = Relative_path.Map.union fast fast_redecl_phase2 in
  let to_recheck = Relative_path.Set.union env.failed_decl to_redecl_phase2 in
  let to_recheck = Relative_path.Set.union to_recheck1 to_recheck in
  let to_recheck = Relative_path.Set.union to_recheck2 to_recheck in
  let hs = SharedMem.heap_size () in
  Hh_logger.log "Heap size: %d" hs;
  HackEventLogger.second_redecl_end t hs;
  let t = Hh_logger.log_duration "Type-decl" t in

  (* TYPE CHECKING *)
  let to_recheck = Relative_path.Set.union to_recheck env.failed_check in
  let fast = extend_fast fast files_info to_recheck in
  ServerCheckpoint.process_updates fast;
  debug_print_fast_keys genv "to_recheck" fast;
  let errorl', err_info =
    Typing_check_service.go genv.workers env.tcopt fast in
  let { Decl_service.
    errs = failed_check;
    lazy_decl_errs = lazy_decl_failed;
  } = err_info in
  let errorl', failed_check = match ServerArgs.ai_mode genv.options with
    | None -> errorl', failed_check
    | Some ai_opt ->
      let fast_infos = reparse_infos files_info fast in
      let ae, af = Ai.go_incremental
        Typing_check_utils.check_defs
        genv.workers fast_infos env.tcopt ai_opt in
      (Errors.merge errorl' ae),
      (Relative_path.Set.union af failed_check)
  in
  let errorl = Errors.merge errorl' errorl in

  let total_rechecked_count = Relative_path.Map.cardinal fast in
  HackEventLogger.type_check_end total_rechecked_count t;
  let t = Hh_logger.log_duration "Type-check" t in

  Hh_logger.log "Total: %f\n%!" (t -. start_t);
  ServerDebug.info genv "incremental_done";

  (* Done, that's the new environment *)
  let new_env = {
    files_info;
    tcopt = env.tcopt;
    errorl = errorl;
    failed_parsing = Relative_path.Set.union failed_naming failed_parsing;
    failed_decl = Relative_path.Set.union failed_decl lazy_decl_failed;
    failed_check = failed_check;
    persistent_client_fd = old_env.persistent_client_fd;
    edited_files = old_env.edited_files;
    files_to_check = SSet.empty;
    diag_subscribe = old_env.diag_subscribe;
    symbols_cache;
  } in
  new_env, total_rechecked_count

(*****************************************************************************)
(* Checks that the working directory is clean *)
(*****************************************************************************)

let check genv env =
  if !debug then begin
    Printf.printf "****************************************\n";
    Printf.printf "Start Check\n";
    flush stdout;
  end;
  type_check genv env
