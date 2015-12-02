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
open Typing_defs
open Utils

module Env    = Typing_env
module SN     = Naming_special_names
module TSubst = Typing_subst
module TUtils = Typing_utils
module Phase  = Typing_phase

type env   = Env.env
type subst = decl ty SMap.t

let make_subst tparams tyl =  TSubst.make tparams tyl

(*****************************************************************************)
(* Code dealing with instantiation. *)
(*****************************************************************************)

let rec instantiate subst env (r, ty: decl ty) =
  (* PERF: If subst is empty then instantiation is a no-op. We can save a
   * significant amount of CPU by avoiding recursively deconstructing the ty
   * data type.
   *)
  if SMap.is_empty subst then env, (r, ty) else
  match ty with
  | Tgeneric (x, cstr_opt) ->
      (match SMap.get x subst with
      | Some x_ty ->
          env, (Reason.Rinstantiate (fst x_ty, x, r), snd x_ty)
      | None -> begin
          match cstr_opt with
          | Some (ck, ty) ->
              let env, ty = instantiate subst env ty in
              env, (r, Tgeneric (x, Some (ck, ty)))
          | None -> env, (r, ty)
        end
      )
  | _ ->
      let env, ty = instantiate_ subst env ty in
      env, (r, ty)

and instantiate_ subst env x =
  match x with
  | Tgeneric _ -> assert false
  (* IMPORTANT: We cannot expand Taccess during instantiation because this can
   * be called before all type consts have been declared and inherited
   *)
  | Taccess (ty, ids) ->
      let env, ty = instantiate subst env ty in
      env, Taccess (ty, ids)
  | Tarray (ty1, ty2) ->
      let env, ty1 = opt (instantiate subst) env ty1 in
      let env, ty2 = opt (instantiate subst) env ty2 in
      env, Tarray (ty1, ty2)
  | Tthis -> env, Tthis
  | Tmixed -> env, Tmixed
  | Tany
  | Tprim _ as x -> env, x
  | Ttuple tyl ->
      let env, tyl = lfold (instantiate subst) env tyl in
      env, Ttuple tyl
  | Toption ty ->
      let env, ty = instantiate subst env ty in
      (* we want to avoid double option: ??T *)
      (match ty with
      | _, Toption _ -> env, snd ty
      | _ -> env, Toption ty
      )
  | Tfun ft ->
      let subst = List.fold_left ~f:begin fun subst (_, (_, x), _) ->
        SMap.remove x subst
      end ~init:subst ft.ft_tparams in
      let env, params = lfold begin fun env (name, param) ->
        let env, param = instantiate subst env param in
        env, (name, param)
      end env ft.ft_params in
      let env, arity = match ft.ft_arity with
        | Fvariadic (min, (name, var_ty)) ->
          let env, var_ty = instantiate subst env var_ty in
          env, Fvariadic (min, (name, var_ty))
        | Fellipsis _ | Fstandard _ as x -> env, x
      in
      let env, ret = instantiate subst env ft.ft_ret in
      env, Tfun { ft with ft_arity = arity; ft_params = params; ft_ret = ret }
  | Tapply (x, tyl) ->
      let env, tyl = lfold (instantiate subst) env tyl in
      env, Tapply (x, tyl)
  | Tshape (fields_known, fdm) ->
      let env, fdm = Nast.ShapeMap.map_env (instantiate subst) env fdm in
      env, Tshape (fields_known, fdm)

let instantiate_ce subst env ({ ce_type = x; _ } as ce) =
  let env, x = instantiate subst env x in
  env, { ce with ce_type = x }

let instantiate_typeconst subst env (
  { ttc_constraint = x; ttc_type = y; _ } as tc) =
    let env, x = opt (instantiate subst) env x in
    let env, y = opt (instantiate subst) env y in
    env, { tc with ttc_constraint = x; ttc_type = y }
