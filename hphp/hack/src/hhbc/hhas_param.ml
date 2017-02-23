(**
 * Copyright (c) 2017, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
*)

type t = {
  param_name      : Litstr.id;
  param_type_info : Hhas_type_info.t option;
}

let make param_name param_type_info =
  { param_name; param_type_info }

let name p = p.param_name
let type_info p = p.param_type_info
