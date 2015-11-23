(**
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 *)

open Utils

let path_of_root root extension =
  (* TODO: move this to places that write this file *)
  Sys_utils.mkdir_no_fail GlobalConfig.tmp_dir;
  let root_part = Path.slash_escaped_string_of_path root in
  Filename.concat GlobalConfig.tmp_dir (spf "%s.%s" root_part extension)

let is_of_root root fn =
  let root_part = Path.slash_escaped_string_of_path root in
  str_starts_with fn (Filename.concat GlobalConfig.tmp_dir root_part)

(* Creates a symlink at <dir>/<linkname.ext> to
 * <dir>/<pluralized ext>/<linkname>-<timestamp>.<ext> *)
let make_link_of_timestamped linkname =
  let open Unix in
  let dir = Filename.dirname linkname in
  Sys_utils.mkdir_no_fail dir;
  let base = Filename.basename linkname in
  let base, ext = Sys_utils.splitext base in
  let dir = Filename.concat dir (spf "%ss" ext) in
  Sys_utils.mkdir_no_fail dir;
  let tm = localtime (time ()) in
  let year = tm.tm_year + 1900 in
  let time_str = spf "%d-%02d-%02d-%02d-%02d-%02d"
    year (tm.tm_mon + 1) tm.tm_mday tm.tm_hour tm.tm_min tm.tm_sec in
  let filename = Filename.concat dir (spf "%s-%s.%s" base time_str ext) in
  Sys_utils.unlink_no_fail linkname;
  Sys_utils.symlink filename linkname;
  filename

(**
 * Lock on this file will be held after the server has finished initializing.
 * *)
let init_complete_file root = path_of_root root "init_complete"
let lock_file root = path_of_root root "lock"
let log_link root = path_of_root root "log"
let pids_file root = path_of_root root "pids"
let socket_file root = path_of_root root "sock"
let dfind_log root = path_of_root root "dfind"
let load_log root = path_of_root root "load"

(** Lock file for server monitor. Locked if monitor is alive.
 * Ensures only 1 server monitor alive per working directory.
 *
 * TODO: Move to a single-lock model, so the monitor and typechecker
 * don't each have one. *)
let server_monitor_liveness_lock root =
  path_of_root root "monitor_liveness_lock"
let server_monitor_log_link root = path_of_root root "monitor_log"
