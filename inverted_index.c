#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <emscripten/emscripten.h>

#define TABLE_SIZE 262139
#define MAX_LINE 1024
#define MAX_WORD 64
#define MAX_QUERY_WORDS 20
#define MAX_POS_OUT 128
#define RESULT_BUF 65536

/* ================= STRUCT DEFINITIONS (Merged from index.h) ================= */

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

static TermNode *hash_table[TABLE_SIZE];

/* ================= HASH ================= */

unsigned int hash(const char *str) {
    unsigned int h = 0;
    while (*str)
        h = (h * 31) + *str++;
    return h % TABLE_SIZE;
}

/* ================= HELPERS ================= */

TermNode* find_term(TermNode *head, const char *term) {
    while (head) {
        if (strcmp(head->term, term) == 0)
            return head;
        head = head->next;
    }
    return NULL;
}

DocNode* find_doc(DocNode *head, int doc_id) {
    while (head) {
        if (head->doc_id == doc_id)
            return head;
        head = head->next;
    }
    return NULL;
}

/* ================= INIT ================= */

EMSCRIPTEN_KEEPALIVE
void index_init() {
    memset(hash_table, 0, sizeof(hash_table));
}

/* ================= INSERT ================= */

void insert_term(const char *term, int doc_id, int pos) {
    unsigned int idx = hash(term);

    TermNode *t = find_term(hash_table[idx], term);
    if (!t) {
        t = malloc(sizeof(TermNode));
        t->term = strdup(term);
        t->docs = NULL;
        t->next = hash_table[idx];
        hash_table[idx] = t;
    }

    DocNode *d = find_doc(t->docs, doc_id);
    if (!d) {
        d = malloc(sizeof(DocNode));
        d->doc_id = doc_id;
        d->positions = NULL;
        d->next = t->docs;
        t->docs = d;
    }

    PosNode *p = malloc(sizeof(PosNode));
    p->pos = pos;
    p->next = d->positions;
    d->positions = p;
}

/* ================= CLEAN WORD ================= */

void clean_word(char *word) {
    char temp[MAX_WORD];
    int j = 0;
    for (int i = 0; word[i]; i++) {
        if (isalpha((unsigned char)word[i]))
            temp[j++] = tolower(word[i]);
    }
    temp[j] = '\0';
    strcpy(word, temp);
}

/* ================= FILE PROCESS ================= */

EMSCRIPTEN_KEEPALIVE
void index_file(const char *filename, int doc_id) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("Error: Could not open file %s in WASM FS\n", filename);
        return;
    }

    char line[MAX_LINE], word[MAX_WORD];
    int pos = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *tok = strtok(line, " \t\n");
        while (tok) {
            strncpy(word, tok, MAX_WORD - 1);
            word[MAX_WORD - 1] = '\0';
            clean_word(word);

            if (strlen(word) > 0)
                insert_term(word, doc_id, pos++);

            tok = strtok(NULL, " \t\n");
        }
    }
    fclose(fp);
}

/* ================= PHRASE ================= */

int collect_phrase_positions(DocNode **docs, int n, int *out, int max) {
    int count = 0;
    PosNode *p0 = docs[0]->positions;

    while (p0) {
        int base = p0->pos;
        int ok = 1;

        for (int i = 1; i < n; i++) {
            PosNode *pi = docs[i]->positions;
            int found = 0;
            while (pi) {
                if (pi->pos == base + i) {
                    found = 1;
                    break;
                }
                pi = pi->next;
            }
            if (!found) { ok = 0; break; }
        }

        if (ok && count < max)
            out[count++] = base;

        p0 = p0->next;
    }
    return count;
}

/* ================= SEARCH ================= */

EMSCRIPTEN_KEEPALIVE
char* search_query_api(const char *query) {
    static char result[RESULT_BUF];
    int offset = 0;

    char qcopy[256];
    strncpy(qcopy, query, sizeof(qcopy)-1);
    qcopy[sizeof(qcopy)-1] = '\0';

    char *words[MAX_QUERY_WORDS];
    int n = 0;

    char *tok = strtok(qcopy, " ");
    while (tok && n < MAX_QUERY_WORDS) {
        clean_word(tok);
        if (strlen(tok) > 0)
            words[n++] = tok;
        tok = strtok(NULL, " ");
    }

    if (n == 0) {
        snprintf(result, RESULT_BUF, "{ \"results\": [] }");
        return result;
    }

    offset += snprintf(result + offset, RESULT_BUF - offset, "{ \"results\": [");

    int first_doc = 1;

    /* ================= SINGLE WORD ================= */
    if (n == 1) {
        TermNode *t = find_term(hash_table[hash(words[0])], words[0]);
        if (!t) {
            snprintf(result + offset, RESULT_BUF - offset, "] }");
            return result;
        }

        for (DocNode *d = t->docs; d; d = d->next) {
            if (!first_doc)
                offset += snprintf(result + offset, RESULT_BUF - offset, ",");

            first_doc = 0;

            offset += snprintf(result + offset, RESULT_BUF - offset,
                               "{\"doc_id\":%d,\"positions\":[",
                               d->doc_id);

            int freq = 0;
            int first_pos = 1;
            for (PosNode *p = d->positions; p; p = p->next) {
                if (!first_pos)
                    offset += snprintf(result + offset, RESULT_BUF - offset, ",");

                first_pos = 0;
                offset += snprintf(result + offset, RESULT_BUF - offset, "%d", p->pos);
                freq++;
            }

            offset += snprintf(result + offset, RESULT_BUF - offset, "],\"frequency\":%d}", freq);
        }
    }

    /* ================= PHRASE SEARCH ================= */
    else {
        TermNode *terms[MAX_QUERY_WORDS];

        for (int i = 0; i < n; i++) {
            terms[i] = find_term(hash_table[hash(words[i])], words[i]);
            if (!terms[i]) {
                snprintf(result + offset, RESULT_BUF - offset, "] }");
                return result;
            }
        }

        for (DocNode *d0 = terms[0]->docs; d0; d0 = d0->next) {
            DocNode *docs[MAX_QUERY_WORDS];
            docs[0] = d0;

            int valid = 1;
            for (int i = 1; i < n; i++) {
                docs[i] = find_doc(terms[i]->docs, d0->doc_id);
                if (!docs[i]) {
                    valid = 0;
                    break;
                }
            }

            if (!valid) continue;

            int phrase_pos[MAX_POS_OUT];
            int freq = collect_phrase_positions(docs, n, phrase_pos, MAX_POS_OUT);

            if (freq > 0) {
                if (!first_doc)
                    offset += snprintf(result + offset, RESULT_BUF - offset, ",");

                first_doc = 0;

                offset += snprintf(result + offset, RESULT_BUF - offset,
                                   "{\"doc_id\":%d,\"positions\":[",
                                   d0->doc_id);

                for (int i = 0; i < freq; i++) {
                    if (i > 0)
                        offset += snprintf(result + offset, RESULT_BUF - offset, ",");

                    offset += snprintf(result + offset, RESULT_BUF - offset, "%d", phrase_pos[i]);
                }

                offset += snprintf(result + offset, RESULT_BUF - offset, "],\"frequency\":%d}", freq);
            }
        }
    }

    snprintf(result + offset, RESULT_BUF - offset, "] }");
    return result;
}

/* ================= CLEAR ================= */

EMSCRIPTEN_KEEPALIVE
void index_clear() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        TermNode *t = hash_table[i];
        while (t) {
            TermNode *tn = t->next;
            DocNode *d = t->docs;
            while (d) {
                DocNode *dn = d->next;
                PosNode *p = d->positions;
                while (p) {
                    PosNode *pn = p->next;
                    free(p);
                    p = pn;
                }
                free(d);
                d = dn;
            }
            free(t->term);
            free(t);
            t = tn;
        }
        hash_table[i] = NULL;
    }
}