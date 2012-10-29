#include "elf32.h"

#include "buffer.h"
#include "index.h"
#include "label.h"
#include "queue.h"
#include "tree.h"
#include "util.h"
#include "wqueue.h"
#include "x86.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <udis86.h>


static const struct _loader_object elf32_object = {
    {
        (void     (*) (void *)) elf32_delete,
        NULL,
        NULL,
        NULL,
        (json_t * (*) (void *)) elf32_serialize
    },
    (uint64_t        (*) (void *))           elf32_entry,
    (struct _graph * (*) (void *))           elf32_graph,
    (struct _tree *  (*) (void *))           elf32_function_tree,
    (struct _map  *  (*) (void *))           elf32_labels,
    (struct _graph * (*) (void *, uint64_t)) elf32_graph_address,
    (struct _map *   (*) (void *))           elf32_memory_map,
    (struct _tree *  (*) (void *, uint64_t)) elf32_function_tree_address,
    (struct _label * (*) (void *, uint64_t)) elf32_label_address
};


struct _elf32 * elf32_create (const char * filename)
{
    FILE * fh;
    size_t filesize;
    struct _elf32 * elf32;

    fh = fopen(filename, "rb");
    if (fh == NULL)
        return NULL;

    fseek(fh, 0, SEEK_END);
    filesize = ftell(fh);
    fseek(fh, 0, SEEK_SET);

    elf32 = (struct _elf32 *) malloc(sizeof(struct _elf32));
    elf32->data = malloc(filesize);
    elf32->loader_object = &elf32_object;

    elf32->data_size = fread(elf32->data, 1, filesize, fh);

    fclose(fh);

    // make sure this is a 32-bit ELF
    if (    (elf32->data_size < 0x200)
         || (elf32->ehdr->e_ident[EI_MAG0]  != ELFMAG0)
         || (elf32->ehdr->e_ident[EI_MAG1]  != ELFMAG1)
         || (elf32->ehdr->e_ident[EI_MAG2]  != ELFMAG2)
         || (elf32->ehdr->e_ident[EI_MAG3]  != ELFMAG3)
         || (elf32->ehdr->e_ident[EI_CLASS] != ELFCLASS32)) {
        elf32_delete(elf32);
        return NULL;       
    }

    return elf32;
}


void elf32_delete (struct _elf32 * elf32)
{
    free(elf32->data);
    free(elf32);
}


json_t * elf32_serialize (struct _elf32 * elf32)
{
    json_t * json = json_object();

    json_object_set(json, "ot", json_integer(SERIALIZE_ELF32));

    json_t * bytes = json_array();
    size_t i;
    for (i = 0; i < elf32->data_size; i++) {
        json_array_append(bytes, json_integer(elf32->data[i]));
    }

    json_object_set(json, "data", bytes);

    return json;
}


struct _elf32 * elf32_deserialize (json_t * json)
{
    json_t * bytes = json_object_get(json, "bytes");

    if (! json_is_array(bytes)) {
        serialize_error = SERIALIZE_ELF32;
        return NULL;
    }

    struct _elf32 * elf32 = (struct _elf32 *) malloc(sizeof(struct _elf32));
    elf32->loader_object  = &elf32_object;
    elf32->data_size      = json_array_size(bytes);
    elf32->data           = (uint8_t *) malloc(elf32->data_size);

    size_t i;
    for (i = 0; i < json_array_size(bytes); i++) {
        json_t * c = json_array_get(bytes, i);
        if (! json_is_integer(c)) {
            serialize_error = SERIALIZE_ELF32;
            free(elf32->data);
            free(elf32);
            return NULL;
        }
        elf32->data[i] = json_integer_value(c);
    }

    return elf32;
}


uint64_t elf32_entry (struct _elf32 * elf32)
{
    return elf32->ehdr->e_entry;
}



uint64_t elf32_base_address (struct _elf32 * elf32)
{
    int phdr_i;

    for (phdr_i = 0; phdr_i < elf32->ehdr->e_phnum; phdr_i++) {
        Elf32_Phdr * phdr = elf32_phdr(elf32, phdr_i);
        if (phdr->p_offset == 0)
            return phdr->p_vaddr;
    }

    return 0;
}



Elf32_Phdr * elf32_phdr (struct _elf32 * elf32, size_t index)
{
    if (elf32->ehdr->e_phoff + ((index + 1) * elf32->ehdr->e_phentsize)
        > elf32->data_size)
        return NULL;

    return (Elf32_Phdr *) &(elf32->data[elf32->ehdr->e_phoff
                                        + (index * elf32->ehdr->e_phentsize)]);
}



Elf32_Shdr * elf32_shdr (struct _elf32 * elf32, size_t index)
{
    if (elf32->ehdr->e_shoff + ((index + 1) * elf32->ehdr->e_shentsize)
        > elf32->data_size)
        return NULL;

    return (Elf32_Shdr *) &(elf32->data[elf32->ehdr->e_shoff
                                        + (index * elf32->ehdr->e_shentsize)]);
}



void * elf32_section_element (struct _elf32 * elf32,
                              size_t section,
                              size_t index)
{
    Elf32_Shdr * shdr = elf32_shdr(elf32, section);
    if (shdr == NULL)
        return NULL;

    if (shdr->sh_offset + ((index + 1) * shdr->sh_entsize) > elf32->data_size)
        return NULL;

    return (Elf32_Sym *) &(elf32->data[shdr->sh_offset
                                       + (index * shdr->sh_entsize)]);
}



char * elf32_strtab_str (struct _elf32 * elf32,
                         unsigned int strtab,
                         unsigned int offset)
{
    Elf32_Shdr * shdr = elf32_shdr(elf32, strtab);
    if (shdr == NULL)
        return NULL;

    if (shdr->sh_offset + offset > elf32->data_size)
        return NULL;

    return (char *) &(elf32->data[shdr->sh_offset + offset]);
}


const char * elf32_sym_name_by_address (struct _elf32 * elf32, uint64_t address)
{
    int shdr_i;
    for (shdr_i = 0; shdr_i < elf32->ehdr->e_shnum; shdr_i++) {
        Elf32_Shdr * shdr = elf32_shdr(elf32, shdr_i);
        if (shdr == NULL)
            break;

        if (shdr->sh_type != SHT_SYMTAB)
            continue;

        int sym_i;
        for (sym_i = 0; sym_i < shdr->sh_size / shdr->sh_entsize; sym_i++) {
            Elf32_Sym * sym = elf32_section_element(elf32, shdr_i, sym_i);
            if (sym == NULL)
                break;

            if (sym->st_value != address)
                continue;
            // found matching symbol
            return elf32_strtab_str(elf32, shdr->sh_link, sym->st_name);
        }
    }

    return NULL;
}



Elf32_Shdr * elf32_shdr_by_name (struct _elf32 * elf32, const char * name)
{
    int i;
    Elf32_Shdr * shdr;

    for (i = 0; i < elf32->ehdr->e_shnum; i++) {
        shdr = elf32_shdr(elf32, i);
        if (shdr == NULL)
            break;

        const char * shdr_name;
        shdr_name = elf32_strtab_str(elf32, 
                                     elf32->ehdr->e_shstrndx,
                                     shdr->sh_name);
        if (shdr_name == NULL)
            break;

        if (strcmp(name, shdr_name) == 0)
            return shdr;
    }

    return NULL;
}



uint64_t elf32_vaddr_to_offset (struct _elf32 * elf32, uint64_t address)
{
    Elf32_Phdr * phdr;
    int i;
    int64_t result;

    for (i = 0; i < elf32->ehdr->e_phnum; i++) {
        phdr = elf32_phdr(elf32, i);
        if (phdr == NULL)
            break;

        if (    (phdr->p_vaddr <= address)
             && (phdr->p_vaddr + phdr->p_filesz >= address)) {
            result = address - phdr->p_vaddr;
            result += phdr->p_offset;
            return result;
        }
    }

    return -1;
}



const char * elf32_rel_name_by_address (struct _elf32 * elf32, uint64_t address)
{
    // find rela sections
    int shdr_i;
    for (shdr_i = 0; shdr_i < elf32->ehdr->e_shnum; shdr_i++) {
        Elf32_Shdr * shdr = elf32_shdr(elf32, shdr_i);
        if (shdr == NULL)
            break;

        if (shdr->sh_type != SHT_REL)
            continue;

        Elf32_Shdr * shdr_sym = elf32_shdr(elf32, shdr->sh_link);
        if (shdr_sym == NULL)
            break;

        int rel_i;
        for (rel_i = 0; rel_i < shdr->sh_size / shdr->sh_entsize; rel_i++) {
            // find an appropriate rela symbol
            Elf32_Rel * rel = elf32_section_element(elf32, shdr_i, rel_i);
            if (rel == NULL)
                break;

            if (rel->r_offset != address)
                continue;
            if (ELF32_R_SYM(rel->r_info) == STN_UNDEF)
                continue;

            // fetch symbol
            Elf32_Sym * sym = elf32_section_element(elf32,
                                                    shdr->sh_link,
                                                    ELF32_R_SYM(rel->r_info));

            if (sym == NULL)
                continue;

            // rela->r_offset needs to be fixed for relocatable objects
            return elf32_strtab_str(elf32, shdr_sym->sh_link, sym->st_name);
        }
    }

    return NULL;
}



struct _graph * elf32_graph (struct _elf32 * elf32)
{

    struct _graph  * graph;
    struct _wqueue * wqueue;

    // disassemble from entry point
    graph = x86_graph(elf32_base_address(elf32),
                      elf32_entry(elf32) - elf32_base_address(elf32),
                      elf32->data,
                      elf32->data_size);

    // disassemble all functions from elf64_function_tree
    struct _tree * function_tree = elf32_function_tree(elf32);
    struct _tree_it * tit;

    wqueue = wqueue_create();
    for (tit = tree_iterator(function_tree);
         tit != NULL;
         tit  = tree_it_next(tit)) {
        struct _index * index = tree_it_data(tit);

        printf("graphing %llx\n", (unsigned long long) index->index);

        struct _x86_graph_wqueue * xgw;
        xgw = x86_graph_wqueue_create(elf32_base_address(elf32),
                                        index->index - elf32_base_address(elf32),
                                        elf32->data,
                                        elf32->data_size);
        wqueue_push(wqueue, WQUEUE_CALLBACK(x86_graph_wqueue), xgw);
        object_delete(xgw);
    }

    wqueue_wait(wqueue);

    while (wqueue_peek(wqueue) != NULL) {
        graph_merge(graph, wqueue_peek(wqueue));
        wqueue_pop(wqueue);
    }

    object_delete(wqueue);

    remove_function_predecessors(graph, function_tree);
    object_delete(function_tree);

    graph_reduce(graph);

    return graph;
}



struct _tree * elf32_function_tree (struct _elf32 * elf32)
{
    struct _tree     * tree = tree_create();

    // add the entry point
    struct _index * index = index_create(elf32_entry(elf32));
    tree_insert(tree, index);
    object_delete(index);

    // recursively disassemble from entry point
    struct _tree * recursive_function_tree;
    recursive_function_tree = x86_functions(elf32_base_address(elf32),
                                            elf32_entry(elf32)
                                            - elf32_base_address(elf32),
                                            elf32->data,
                                            elf32->data_size);

    struct _tree_it * it;
    for (it = tree_iterator(recursive_function_tree);
         it != NULL;
         it = tree_it_next(it)) {
        struct _index * index = tree_it_data(it);
        if (tree_fetch(tree, index) == NULL)
            tree_insert(tree, index);
    }

    object_delete(recursive_function_tree);

    // symbols are easy
    int sec_i;
    for (sec_i = 0; sec_i < elf32->ehdr->e_shnum; sec_i++) {
        Elf32_Shdr * shdr = elf32_shdr(elf32, sec_i);
        if (shdr == NULL)
            break;

        if (shdr->sh_type != SHT_SYMTAB)
            continue;

        int sym_i;
        for (sym_i = 0; sym_i < shdr->sh_size / shdr->sh_entsize; sym_i++) {
            Elf32_Sym * sym = elf32_section_element(elf32, sec_i, sym_i);
            if (sym == NULL)
                break;

            if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC)
                continue;

            if (sym->st_value == 0)
                continue;

            struct _index * index = index_create(sym->st_value);
            if (tree_fetch(tree, index) == NULL)
                tree_insert(tree, index);
                
            object_delete(index);
        }
    }

    // check for __libc_start_main loader
    uint64_t target_offset = elf32_entry(elf32) - elf32_base_address(elf32) + 0x17;
    if (target_offset + 0x10 > elf32->data_size)
        return tree;

    uint8_t * data = &(elf32->data[target_offset]);
    size_t    size = elf32->data_size - target_offset;

    ud_t ud_obj;
    ud_init      (&ud_obj);
    ud_set_mode  (&ud_obj, 32);
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_input_buffer(&ud_obj, data, size);
    ud_disassemble(&ud_obj);
    if (ud_obj.mnemonic == UD_Ipush) {
        printf("found __libc_start_main loader, main at %llx\n",
           (unsigned long long) udis86_sign_extend_lval(&(ud_obj.operand[0])));

        // add main to function tree
        struct _index * index;
        index = index_create(udis86_sign_extend_lval(&(ud_obj.operand[0])));
        if (tree_fetch(tree, index) == NULL)
            tree_insert(tree, index);
        object_delete(index);

        struct _tree * recursive_function_tree;
        recursive_function_tree = x86_functions(elf32_base_address(elf32),
                                  udis86_sign_extend_lval(&(ud_obj.operand[0]))
                                   - elf32_base_address(elf32),
                                                  elf32->data,
                                                  elf32->data_size);
        struct _tree_it * it;
        for (it = tree_iterator(recursive_function_tree);
             it != NULL;
             it = tree_it_next(it)) {
            struct _index * index = tree_it_data(it);
            if (tree_fetch(tree, index) == NULL)
                tree_insert(tree, index);
        }
        object_delete(recursive_function_tree);
    }
    else
        printf("disassembled: %s\n disassembled at %llx\n",
               ud_insn_asm(&ud_obj),
               (unsigned long long) target_offset);

    return tree;
}



struct _map * elf32_labels (struct _elf32 * elf32)
{

    struct _map * labels_map = map_create();

    struct _tree * function_tree = elf32_function_tree(elf32);
    struct _tree_it * it;
    for (it = tree_iterator(function_tree); it != NULL; it = tree_it_next(it)) {
        struct _index * index = tree_it_data(it);

        struct _label * label = elf32_label_address(elf32, index->index);
        map_insert(labels_map, index->index, label);
        object_delete(label);
    }

    object_delete(function_tree);
    return labels_map;
}


struct _graph * elf32_graph_address (struct _elf32 * elf32, uint64_t address)
{
    struct _graph * graph;

    graph = x86_graph(elf32_base_address(elf32),
                      address - elf32_base_address(elf32),
                      elf32->data,
                      elf32->data_size);

    graph_reduce(graph);
    return graph;
}


struct _map * elf32_memory_map (struct _elf32 * elf32)
{
    struct _map * map = map_create();

    int phdr_i;
    for (phdr_i = 0; phdr_i < elf32->ehdr->e_phnum; phdr_i++) {
        Elf32_Phdr * phdr = elf32_phdr(elf32, phdr_i);

        uint64_t bottom = phdr->p_vaddr;
        uint64_t top    = phdr->p_vaddr + phdr->p_memsz;

        if (top - bottom == 0)
            continue;

        uint8_t * tmp = malloc(phdr->p_memsz);
        memset(tmp, 0, phdr->p_memsz);
        memcpy(tmp, &(elf32->data[phdr->p_offset]), phdr->p_filesz);

        struct _buffer * buffer;
        uint64_t key;
        // do we already have a buffer that this section overlaps?
        buffer = map_fetch_max(map, phdr->p_vaddr + phdr->p_memsz);
        key    = map_fetch_max_key(map, phdr->p_vaddr + phdr->p_memsz);

        if (    (buffer != NULL)
             && (    ((bottom <= key) && (top >= key))
                  || ((bottom <= key + buffer->size) && (top >= key + buffer->size))
                  || ((bottom >= key) && (top <= key + buffer->size)))) {
            // create a temporary buffer to hold this sections contents.

            // if this section fits inside a previous section, then modify in place
            if ((bottom >= key) && (top <= key + buffer->size)) {
                memcpy(&(buffer->bytes[bottom - key]), tmp, phdr->p_memsz);
            }
            // if this section comes before a previous section (or contains
            // previous section)
            else if (bottom <= key) {
                uint64_t new_size;
                new_size = ((key + buffer->size) > top ? (key + buffer->size) : top);
                new_size -= bottom;
                uint8_t * tmp2 = malloc(new_size);
                memcpy(&(tmp2[key - bottom]), buffer->bytes, buffer->size);
                memcpy(tmp2, tmp, phdr->p_memsz);
                struct _buffer * new_buffer = buffer_create(tmp2, new_size);
                map_remove(map, key);
                map_insert(map, bottom, new_buffer);
                object_delete(new_buffer);
                free(tmp2);
            }
            // if this section overlaps previous section but starts after
            // previous section starts
            else {
                uint64_t new_size = top - key;
                uint8_t * tmp2 = malloc(new_size);
                memcpy(tmp2, buffer->bytes, buffer->size);
                memcpy(&(tmp2[bottom - key]), tmp, phdr->p_memsz);
                struct _buffer * new_buffer = buffer_create(tmp2, new_size);
                map_remove(map, key);
                map_insert(map, key, new_buffer);
                object_delete(new_buffer);
                free(tmp2);
            }

        }
        // we don't have a previous section that this buffer overlaps
        else {
            struct _buffer * new_buffer = buffer_create(tmp, top - bottom);
            map_insert(map, bottom, new_buffer);
            object_delete(new_buffer);
        }

        free(tmp);
    }

    return map;
}


struct _tree * elf32_function_tree_address (struct _elf32 * elf32, uint64_t address)
{
    struct _tree * tree = x86_functions(elf32_base_address(elf32),
                                        address - elf32_base_address(elf32),
                                        elf32->data,
                                        elf32->data_size);

    return tree;
}


struct _label * elf32_label_address (struct _elf32 * elf32, uint64_t address)
{
    Elf32_Shdr * plt_shdr = elf32_shdr_by_name(elf32, ".plt");
    uint64_t plt_bottom;
    uint64_t plt_top;

    if (plt_shdr == NULL) {
        plt_bottom = -1;
        plt_top = -1;
    }
    else {
        plt_bottom = plt_shdr->sh_addr;
        plt_top    = plt_bottom + plt_shdr->sh_size;
    }
    

    // plt functions are a special case, as we try to identify their targets
    // in the got
    // address is within the plt
    if (    (address >= plt_bottom)
         && (address <  plt_top)) {

        // disassemble instruction
        uint8_t * data  = &(elf32->data[address - elf32_base_address(elf32)]);
        ud_t ud_obj;
        ud_init(&ud_obj);
        ud_set_mode  (&ud_obj, 32);
        ud_set_input_buffer(&ud_obj, data, 0x20);
        ud_disassemble(&ud_obj);

        if (    (ud_obj.mnemonic == UD_Ijmp)
             && (udis86_sign_extend_lval(&(ud_obj.operand[0])) != -1)) {
            uint64_t target = udis86_sign_extend_lval(&(ud_obj.operand[0]));
            const char * name = elf32_rel_name_by_address(elf32, target);
            if (name != NULL) {
                char plttmp[256];
                snprintf(plttmp, 256, "%s@plt", name);
                struct _label * label;
                label = label_create(address, plttmp, LABEL_FUNCTION);
                return label;
            }
        }
    }

    // look for a symbol
    const char * name = elf32_sym_name_by_address(elf32, address);
    // no symbol
    if ((name == NULL) || (strcmp(name, "") == 0)) {
        char tmp[128];
        snprintf(tmp, 128, "fun_%llx", (unsigned long long) address);
        name = tmp;
    }
    // symbol exists
    struct _label * label = label_create(address, name, LABEL_FUNCTION);
    return label;
}