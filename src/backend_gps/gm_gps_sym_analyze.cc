
#include <stdio.h>
#include "gm_backend_gps.h"
#include "gm_error.h"
#include "gm_code_writer.h"
#include "gm_frontend.h"
#include "gm_transform_helper.h"
#include "gm_typecheck.h"
#include "gps_syminfo.h"


#include <set>


bool gm_gps_gen::do_analyze_symbols()
{
    bool is_okay = true;

    // first, check in which BB each symbol is used and how
    do_merge_symbol_usages();

    // second, make a big flat list of symbols
    do_make_symbol_summary();

    return is_okay;
}




class gps_merge_symbol_usage_t : public gps_apply_bb_ast  
{
    static bool const IS_SCALAR = true;
    static bool const IS_FIELD  = false;

    public:
        gps_merge_symbol_usage_t() {
            set_for_sent(true);
            set_for_expr(true);
        }
    
    virtual bool apply(ast_sent* s) 
    {
        // only need to look at assign statement (for LHS)
        // (RHS usages will be gathered in apply(expr)
        if (s->get_nodetype() == AST_ASSIGN)
        {
            ast_assign * a = (ast_assign*) s;
            // check LHS
            if (a->is_target_scalar())
            {
                ast_id* i = a->get_lhs_scala();
                bool is_reduce = a->is_reduce_assign();
                if (is_reduce) {
                    update_access_information(i, IS_SCALAR, GPS_SYM_USED_AS_REDUCE,
                            a->get_reduce_type());
                }
                else {
                    update_access_information(i, IS_SCALAR, GPS_SYM_USED_AS_LHS);
                }
            }
            else {
                ast_id* i = a->get_lhs_field()->get_second();
                bool is_reduce = a->is_reduce_assign();
                if (is_reduce) {
                    update_access_information(i, IS_FIELD, GPS_SYM_USED_AS_REDUCE,
                            a->get_reduce_type());
                }
                else {
                    update_access_information(i, IS_FIELD, GPS_SYM_USED_AS_LHS);
                }

            }
        }
    }

    virtual bool apply(ast_expr* e) 
    {
        // RHS
        if (e->is_id()) {
            ast_id* i = e->get_id();
            update_access_information(i, IS_SCALAR, GPS_SYM_USED_AS_RHS);
        } 
        else if (e->is_field()) {
            // [XXX] to be done
            ast_id* i = e->get_field()->get_second();
            update_access_information(i, IS_FIELD, GPS_SYM_USED_AS_RHS);
        }
    }
    protected:
        void update_access_information(ast_id *i, bool is_scalar, int usage, int r_type = GMREDUCE_NULL)
        {
            // update global information
            gps_syminfo* syminfo = 
                get_or_create_global_syminfo(i, is_scalar);

            // update global information
            syminfo->add_usage_in_BB(
                    get_curr_BB()->get_id(), 
                    usage,
                    get_curr_BB()->is_vertex(),
                    r_type);

            // update local information
            syminfo = 
                get_or_create_local_syminfo(i, is_scalar);

            syminfo->add_usage_in_BB(
                    get_curr_BB()->get_id(), 
                    usage,
                    get_curr_BB()->is_vertex(),
                    r_type);

        }

        gps_syminfo* get_or_create_global_syminfo(ast_id *i, bool is_scalar)
        {
            gm_symtab_entry* sym = i->getSymInfo(); 

            ast_extra_info* info = sym->find_info(TAG_BB_USAGE);
            gps_syminfo* syminfo;
            if (info == NULL)  {
                syminfo = new gps_syminfo(is_scalar);
                sym->add_info(TAG_BB_USAGE, syminfo);
            } else {
                syminfo = (gps_syminfo*) info;
            }
            return syminfo;
        }

        gps_syminfo* get_or_create_local_syminfo(ast_id *i, bool is_scalar)
        {
            gm_symtab_entry* sym = i->getSymInfo(); 

            // find info from BB-local map
            gps_syminfo* syminfo =
                get_curr_BB()-> find_symbol_info(sym);
            if (syminfo == NULL)  {
                syminfo = new gps_syminfo(is_scalar);
                get_curr_BB()->add_symbol_info(sym, syminfo);
            }
            return syminfo;
        }
};


bool gm_gps_gen::do_merge_symbol_usages()
{
    ast_procdef* p = get_current_proc();
    gm_gps_basic_block* entry_BB = get_entry_basic_block();
    assert(p!= NULL);
    assert(entry_BB!=NULL);

    // traverse BB
    gps_merge_symbol_usage_t T;
    gps_bb_traverse_ast(entry_BB, &T, false, true);

    return true;

}

class gps_flat_symbol_t : public gm_apply
{
public:
    gps_flat_symbol_t(
        std::set<gm_symtab_entry* >& s,
        std::set<gm_symtab_entry* >& p)
        : scalar(s), prop(p)
    {
        set_for_symtab(true);
    }


    virtual bool apply(gm_symtab_entry* sym, int symtab_type)
    {
        ast_extra_info* info = sym->find_info(TAG_BB_USAGE);
        if (info == NULL) return true;

        gps_syminfo* syminfo = (gps_syminfo*) info;
        if (syminfo->is_scalar())
        {
            if (symtab_type == GM_SYMTAB_ARG)
            {
                syminfo->set_is_argument(true);
                scalar.insert(sym);
            }
            else if (syminfo->is_used_in_multiple_BB()){
                scalar.insert(sym);
            } else {
                // ignore?
            }
        }
        else
        {
            // should be set externally.
            if (syminfo->is_argument())
            {
                prop.insert(sym);
            }
            else if (syminfo->is_used_in_multiple_BB()){
                prop.insert(sym);
            } else {
                // ignore?
            }
        }

        return true;
    }
private:
    std::set<gm_symtab_entry* >& scalar;
    std::set<gm_symtab_entry* >& prop;
};


bool gm_gps_gen::do_make_symbol_summary()
{
    ast_procdef* p = get_current_proc();
    //-----------------------------------------------
    // mark special markers to the property arguments
    //-----------------------------------------------
    std::vector<gm_symtab_entry* >::iterator J;
    std::vector<gm_symtab_entry*>& args =
        p->get_symtab_field()->get_entries(); 
    for(J=args.begin(); J!= args.end(); J++)
    {
        gm_symtab_entry *sym = *J; 
        gps_syminfo* syminfo = (gps_syminfo*) 
            sym->find_info(TAG_BB_USAGE);
        assert(syminfo!=NULL);
        syminfo->set_is_argument(true);
    }


    //-------------------------------------
    // make a flat symbol table
    //-------------------------------------
    gps_flat_symbol_t T(scalar, prop);
    p->traverse(&T, false, true);

    //-------------------------------
    // order property symbols
    // (todo: opt for cacheline ?)
    //-------------------------------
    int byte_begin = 0;
    std::set<gm_symtab_entry* >::iterator I;
    for(I=prop.begin(); I!=prop.end(); I++)
    {
        gm_symtab_entry * sym = *I; 
        gps_syminfo* syminfo = (gps_syminfo*) sym->find_info(TAG_BB_USAGE);

        int size = get_lib()->get_type_size(sym->getType()->get_target_type());
        syminfo->set_start_byte(byte_begin);
        byte_begin += size;
    }
    set_total_property_size(byte_begin);

    return true;
}