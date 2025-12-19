#ifndef INDEX_H
#define INDEX_H

typedef struct PosNode {
    int pos;
    struct PosNode *next;
} PosNode;

typedef struct DocNode {
    int doc_id;
    PosNode *positions;
    struct DocNode *next;
} DocNode;

typedef struct TermNode {
    char *term;
    DocNode *docs;
    struct TermNode *next;
} TermNode;

/* API */
void index_init();
void index_file(const char *filename, int doc_id);
char* search_query_api(const char *query);
void index_clear();

#endif
