#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

typedef struct {
    const uint8_t* data;
    size_t size;

    uint16_t units_per_em;

    const uint8_t* cmap;
    const uint8_t* hmtx;

    uint16_t num_glyphs;
    uint16_t num_hmetrics;

    uint32_t cmap_offset;
    uint32_t hmtx_offset;
} TTF_Font;

typedef uint16_t glyph;

static uint16_t ttf_u16(const uint8_t* p) {
    return (p[0] << 8) | p[1];
}

static uint32_t ttf_u32(const uint8_t* p) {
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

static const uint8_t* ttf_find_table(const uint8_t* data, const char tag[4]) {
    uint16_t num_tables = ttf_u16(data + 4);
    const uint8_t* dir = data + 12;

    for (int i = 0; i < num_tables; i++) {
        const uint8_t* rec = dir + i * 16;
        if (rec[0]==tag[0] && rec[1]==tag[1] &&
            rec[2]==tag[2] && rec[3]==tag[3]) {
            uint32_t offset = ttf_u32(rec + 8);
            return data + offset;
        }
    }
    return NULL;
}

static int ttf_init(TTF_Font* f, const uint8_t* data, size_t size) {
    f->data = data;
    f->size = size;

    const uint8_t* head = ttf_find_table(data, "head");
    const uint8_t* hhea = ttf_find_table(data, "hhea");
    const uint8_t* maxp = ttf_find_table(data, "maxp");
    const uint8_t* hmtx = ttf_find_table(data, "hmtx");
    const uint8_t* cmap = ttf_find_table(data, "cmap");

    if (!head || !hhea || !maxp || !hmtx || !cmap) return 0;

    f->units_per_em = ttf_u16(head + 18);
    f->num_glyphs   = ttf_u16(maxp + 4);
    f->num_hmetrics = ttf_u16(hhea + 34);

    f->hmtx = hmtx;
    f->cmap = cmap;

    return 1;
}

static uint16_t ttf_glyph_index(TTF_Font* f, uint32_t codepoint) {
    const uint8_t* cmap = f->cmap;

    uint16_t num_tables = ttf_u16(cmap + 2);
    const uint8_t* rec = cmap + 4;

    for (int i = 0; i < num_tables; i++) {
        uint16_t platform = ttf_u16(rec + i * 8 + 0);
        uint32_t offset   = ttf_u32(rec + i * 8 + 4);

        // prefer Windows Unicode
        if (platform == 3) {
            const uint8_t* sub = cmap + offset;
            uint16_t format = ttf_u16(sub + 0);

            if (format == 4) {
                uint16_t segCount = ttf_u16(sub + 6) / 2;

                const uint8_t* endCode   = sub + 14;
                const uint8_t* startCode = endCode + 2 + segCount * 2;
                const uint8_t* idDelta   = startCode + segCount * 2;
                const uint8_t* idRange   = idDelta + segCount * 2;

                for (int s = 0; s < segCount; s++) {
                    uint16_t end = ttf_u16(endCode + s * 2);
                    uint16_t start = ttf_u16(startCode + s * 2);

                    if (codepoint >= start && codepoint <= end) {
                        uint16_t delta = ttf_u16(idDelta + s * 2);
                        uint16_t range = ttf_u16(idRange + s * 2);

                        if (range == 0) {
                            return (uint16_t)((codepoint + delta) & 0xFFFF);
                        } else {
                            const uint8_t* glyph =
                                idRange + s * 2 + range + (codepoint - start) * 2;
                            uint16_t glyph_id = ttf_u16(glyph);
                            if (!glyph_id) {
                                return 0;
                            }
                            return (uint16_t)((glyph_id + delta) & 0xFFFF);
                        }
                    }
                }
            }

            if (format == 12) {
                uint32_t nGroups = ttf_u32(sub + 12);
                const uint8_t* groups = sub + 16;

                for (uint32_t g = 0; g < nGroups; g++) {
                    const uint8_t* grp = groups + g * 12;

                    uint32_t startChar = ttf_u32(grp + 0);
                    uint32_t endChar   = ttf_u32(grp + 4);
                    uint32_t startGID  = ttf_u32(grp + 8);

                    if (codepoint >= startChar && codepoint <= endChar) {
                        return (uint16_t)(startGID + (codepoint - startChar));
                    }
                }
            }
        }
    }

    return 0;
}

static uint16_t ttf_advance_width(TTF_Font* f, uint16_t glyph) {
    if (glyph < f->num_hmetrics) {
        return ttf_u16(f->hmtx + glyph * 4);
    } else {
        return ttf_u16(f->hmtx + (f->num_hmetrics - 1) * 4);
    }
}

typedef struct {
    FILE* fp;
    long offsets[16];
    long content_start;
    int obj_count;
} PDF;

#define PDF_CATALOG_ID 1
#define PDF_PAGES_ID 2
#define PDF_FIRST_PAGE_ID 3
#define PDF_FONT_ID 4
#define PDF_FIRST_CONTENT_ID 5
#define PDF_CID_FONT_ID 6
#define PDF_FONT_DESC_ID 7
#define PDF_FONT_FILE_ID 8
#define PDF_TEXT_FONT_ID 9
#define PDF_TEXT_CID_FONT_ID 10
#define PDF_TEXT_FONT_DESC_ID 11
#define PDF_TEXT_FONT_FILE_ID 12
#define PDF_NEXT_DYNAMIC_ID 13

typedef struct {
    int page_id;
    int content_id;
} PDF_Page;

static FILE* pdf_fp;
static long* offsets;
static int offsets_capacity;
static int current_content_id;
static PDF_Page* pages;
static int page_count;
static int page_capacity;
static int next_obj_id;
static TTF_Font g_font;
static uint8_t* g_font_data;
static size_t g_font_size;
static TTF_Font g_text_font;
static uint8_t* g_text_font_data;
static size_t g_text_font_size;

/* Content stream is buffered in memory so that XObjects (images, etc.)
   can be written safely to pdf_fp between pages without corrupting the
   stream. Drawing functions write to content_fp; pdf_finish_current_page
   flushes the buffer to pdf_fp as a complete, correctly-lengthed object. */
static FILE*   content_fp          = NULL;
static char*   content_buf         = NULL;
static size_t  content_buf_size    = 0;

typedef struct {
    int obj_id;
    int width;
    int height;
} PDF_Image;

static PDF_Image* pdf_images = NULL;
static int pdf_image_count = 0;
static int pdf_image_capacity = 0;

// ---------- helpers ----------
static void pdf_out_of_memory() {
    fprintf(stderr, "out of memory\n");
    exit(1);
}

static void pdf_ensure_offset(int id) {
    int old_capacity = offsets_capacity;
    int new_capacity = offsets_capacity ? offsets_capacity : 16;
    long* new_offsets;

    if (id < offsets_capacity) {
        return;
    }

    while (new_capacity <= id) {
        new_capacity *= 2;
    }

    new_offsets = (long*)realloc(offsets, sizeof(long) * (size_t)new_capacity);
    if (!new_offsets) {
        pdf_out_of_memory();
    }

    offsets = new_offsets;
    for (int i = old_capacity; i < new_capacity; i++) {
        offsets[i] = 0;
    }
    offsets_capacity = new_capacity;
}

static void pdf_add_page(int page_id, int content_id) {
    if (page_count == page_capacity) {
        int new_capacity = page_capacity ? page_capacity * 2 : 8;
        PDF_Page* new_pages =
            (PDF_Page*)realloc(pages, sizeof(PDF_Page) * (size_t)new_capacity);

        if (!new_pages) {
            pdf_out_of_memory();
        }

        pages = new_pages;
        page_capacity = new_capacity;
    }

    pages[page_count].page_id = page_id;
    pages[page_count].content_id = content_id;
    page_count++;
}

static void pdf_begin_obj(int id) {
    pdf_ensure_offset(id);
    offsets[id] = ftell(pdf_fp);
    fprintf(pdf_fp, "%d 0 obj\n", id);
}

static void pdf_end_obj() {
    fprintf(pdf_fp, "endobj\n");
}

static int pdf_load_ttf(const char* path, TTF_Font* font_info,
                        uint8_t** font_data, size_t* font_size) {
    FILE* font = fopen(path, "rb");
    if (!font) {
        return 0;
    }

    fseek(font, 0, SEEK_END);
    long size = ftell(font);
    rewind(font);

    if (size <= 0) {
        fclose(font);
        return 0;
    }

    *font_size = (size_t)size;
    *font_data = (uint8_t*)malloc(*font_size);

    if (!*font_data) {
        fclose(font);
        return 0;
    }

    if (fread(*font_data, 1, *font_size, font) != *font_size) {
        fclose(font);
        free(*font_data);
        *font_data = NULL;
        return 0;
    }

    fclose(font);

    if (!ttf_init(font_info, *font_data, *font_size)) {
        free(*font_data);
        *font_data = NULL;
        return 0;
    }

    return 1;
}

static void pdf_write_font_objects(int type0_id, int cid_id, int desc_id,
                                   int file_id, const char* base_name,
                                   const uint8_t* font_data,
                                   size_t font_size) {
    pdf_begin_obj(type0_id);
    fprintf(pdf_fp,
        "<< /Type /Font\n"
        "   /Subtype /Type0\n"
        "   /BaseFont /%s\n"
        "   /Encoding /Identity-H\n"
        "   /DescendantFonts [%d 0 R]\n"
        ">>\n",
        base_name,
        cid_id);
    pdf_end_obj();

    pdf_begin_obj(cid_id);
    fprintf(pdf_fp,
        "<< /Type /Font\n"
        "   /Subtype /CIDFontType2\n"
        "   /BaseFont /%s\n"
        "   /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>\n"
        "   /FontDescriptor %d 0 R\n"
        "   /CIDToGIDMap /Identity\n"
        "   /W [0 [500]]\n"
        ">>\n",
        base_name,
        desc_id);
    pdf_end_obj();

    pdf_begin_obj(desc_id);
    fprintf(pdf_fp,
        "<< /Type /FontDescriptor\n"
        "   /FontName /%s\n"
        "   /Flags 32\n"
        "   /FontBBox [0 -200 1000 800]\n"
        "   /Ascent 800\n"
        "   /Descent -200\n"
        "   /CapHeight 700\n"
        "   /ItalicAngle 0\n"
        "   /StemV 80\n"
        "   /FontFile2 %d 0 R\n"
        ">>\n",
        base_name,
        file_id);
    pdf_end_obj();

    pdf_begin_obj(file_id);
    fprintf(pdf_fp, "<< /Length %zu >>\nstream\n", font_size);
    fwrite(font_data, 1, font_size, pdf_fp);
    fprintf(pdf_fp, "\nendstream\n");
    pdf_end_obj();
}

static uint32_t utf8_next_codepoint(const char** text) {
    const unsigned char* s = (const unsigned char*)*text;
    uint32_t codepoint = 0;

    if (s[0] < 0x80) {
        *text += 1;
        return s[0];
    }

    if ((s[0] & 0xE0) == 0xC0 &&
        s[1] &&
        (s[1] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x1F) << 6) |
                    (uint32_t)(s[1] & 0x3F);
        *text += 2;
        return codepoint;
    }

    if ((s[0] & 0xF0) == 0xE0 &&
        s[1] &&
        s[2] &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x0F) << 12) |
                    ((uint32_t)(s[1] & 0x3F) << 6) |
                    (uint32_t)(s[2] & 0x3F);
        *text += 3;
        return codepoint;
    }

    if ((s[0] & 0xF8) == 0xF0 &&
        s[1] &&
        s[2] &&
        s[3] &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x07) << 18) |
                    ((uint32_t)(s[1] & 0x3F) << 12) |
                    ((uint32_t)(s[2] & 0x3F) << 6) |
                    (uint32_t)(s[3] & 0x3F);
        *text += 4;
        return codepoint;
    }

    *text += 1;
    return '?';
}

static float font_glyph_width(TTF_Font* font_info, uint16_t gid, float size) {
    if (!gid || !font_info->units_per_em) {
        return 0.0f;
    }

    return (float)ttf_advance_width(font_info, gid) *
           size /
           (float)font_info->units_per_em;
}

static void pdf_finish_current_page() {
    if (!current_content_id) {
        return;
    }

    /* Close the memstream — content_buf and content_buf_size are now valid. */
    fclose(content_fp);
    content_fp = NULL;

    /* Write the complete content object to the file now that we know the length. */
    pdf_begin_obj(current_content_id);
    fprintf(pdf_fp, "<< /Length %zu >>\nstream\n", content_buf_size);
    fwrite(content_buf, 1, content_buf_size, pdf_fp);
    fprintf(pdf_fp, "\nendstream\n");
    pdf_end_obj();

    free(content_buf);
    content_buf = NULL;
    content_buf_size = 0;
    current_content_id = 0;
}

static void pdf_start_page() {
    int page_id;
    int content_id;

    if (page_count == 0) {
        page_id = PDF_FIRST_PAGE_ID;
        content_id = PDF_FIRST_CONTENT_ID;
    } else {
        page_id = next_obj_id++;
        content_id = next_obj_id++;
    }

    pdf_add_page(page_id, content_id);
    current_content_id = content_id;

    /* Open an in-memory stream for this page's drawing commands.
       pdf_embed_image (and anything else that needs to write PDF objects)
       writes directly to pdf_fp, which is safe because no content object
       is open on disk at this point. */
    content_fp = open_memstream(&content_buf, &content_buf_size);
    if (!content_fp) {
        fprintf(stderr, "out of memory opening content stream\n");
        exit(1);
    }

    fprintf(content_fp, "0 0 0 rg\n");
    fprintf(content_fp, "0 0 0 RG\n");
}

static void pdf_new_page() {
    pdf_finish_current_page();
    pdf_start_page();
}

static void pdf_write_page_objects() {
    for (int i = 0; i < page_count; i++) {
        pdf_begin_obj(pages[i].page_id);
        fprintf(pdf_fp,
            "<< /Type /Page\n"
            "   /Parent %d 0 R\n"
            "   /MediaBox [0 0 612 792]\n"
            "   /Resources << /Font << /F1 %d 0 R /F2 %d 0 R >>",
            PDF_PAGES_ID,
            PDF_FONT_ID,
            PDF_TEXT_FONT_ID);
        if (pdf_image_count > 0) {
            fprintf(pdf_fp, "\n              /XObject <<");
            for (int j = 0; j < pdf_image_count; j++) {
                fprintf(pdf_fp, " /Im%d %d 0 R",
                        pdf_images[j].obj_id, pdf_images[j].obj_id);
            }
            fprintf(pdf_fp, " >>");
        }
        fprintf(pdf_fp,
            " >>\n"
            "   /Contents %d 0 R\n"
            ">>\n",
            pages[i].content_id);
        pdf_end_obj();
    }
}

static void pdf_write_pages_object() {
    pdf_begin_obj(PDF_PAGES_ID);
    fprintf(pdf_fp, "<< /Type /Pages /Kids [");
    for (int i = 0; i < page_count; i++) {
        fprintf(pdf_fp, "%d 0 R ", pages[i].page_id);
    }
    fprintf(pdf_fp, "] /Count %d >>\n", page_count);
    pdf_end_obj();
}

static void begin_document(const char* filename) {
    const char* font_path = "cmu.serif-roman.ttf";

    pdf_fp = fopen(filename, "wb");
    if (!pdf_fp) {
        fprintf(stderr, "could not open %s\n", filename);
        exit(1);
    }

    offsets = NULL;
    offsets_capacity = 0;
    pages = NULL;
    page_count = 0;
    page_capacity = 0;
    current_content_id = 0;
    next_obj_id = PDF_NEXT_DYNAMIC_ID;
    content_fp = NULL;
    content_buf = NULL;
    content_buf_size = 0;

    if (!pdf_load_ttf(font_path, &g_font, &g_font_data, &g_font_size)) {
        fprintf(stderr, "could not load %s\n", font_path);
        fclose(pdf_fp);
        pdf_fp = NULL;
        exit(1);
    }

    if (!pdf_load_ttf(font_path, &g_text_font, &g_text_font_data, &g_text_font_size)) {
        fprintf(stderr, "could not load %s for text\n", font_path);
        free(g_font_data);
        g_font_data = NULL;
        fclose(pdf_fp);
        pdf_fp = NULL;
        exit(1);
    }

    fprintf(pdf_fp, "%%PDF-1.4\n");

    pdf_begin_obj(PDF_CATALOG_ID);
    fprintf(pdf_fp, "<< /Type /Catalog /Pages %d 0 R >>\n", PDF_PAGES_ID);
    pdf_end_obj();

    pdf_write_font_objects(PDF_FONT_ID,
                           PDF_CID_FONT_ID,
                           PDF_FONT_DESC_ID,
                           PDF_FONT_FILE_ID,
                           "",
                           g_font_data, g_font_size);
    pdf_write_font_objects(PDF_TEXT_FONT_ID,
                           PDF_TEXT_CID_FONT_ID,
                           PDF_TEXT_FONT_DESC_ID,
                           PDF_TEXT_FONT_FILE_ID,
                           "BravuraText",
                           g_text_font_data, g_text_font_size);
    pdf_start_page();
}

static int pdf_embed_image(const char* path, int* out_w, int* out_h) {
    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 3);
    if (!pixels) {
        fprintf(stderr, "~image: stb_image could not load '%s': %s\n",
                path, stbi_failure_reason());
        return 0;
    }

    /* Compress the raw RGB row data with zlib deflate. */
    uLong raw_size = (uLong)w * (uLong)h * 3;
    uLong compressed_bound = compressBound(raw_size);
    Bytef* compressed = (Bytef*)malloc(compressed_bound);
    if (!compressed) {
        stbi_image_free(pixels);
        fprintf(stderr, "~image: out of memory compressing '%s'\n", path);
        return 0;
    }

    uLong compressed_size = compressed_bound;
    int zret = compress2(compressed, &compressed_size,
                         (const Bytef*)pixels, raw_size, Z_DEFAULT_COMPRESSION);
    stbi_image_free(pixels);

    if (zret != Z_OK) {
        free(compressed);
        fprintf(stderr, "~image: zlib compress failed (code %d) for '%s'\n", zret, path);
        return 0;
    }

    int img_id = next_obj_id++;
    pdf_ensure_offset(img_id);
    offsets[img_id] = ftell(pdf_fp);
    fprintf(pdf_fp,
        "%d 0 obj\n"
        "<< /Type /XObject /Subtype /Image\n"
        "   /Width %d /Height %d\n"
        "   /ColorSpace /DeviceRGB\n"
        "   /BitsPerComponent 8\n"
        "   /Filter /FlateDecode\n"
        "   /Length %lu\n"
        ">>\nstream\n",
        img_id, w, h, (unsigned long)compressed_size);
    fwrite(compressed, 1, compressed_size, pdf_fp);
    free(compressed);
    fprintf(pdf_fp, "\nendstream\nendobj\n");

    /* Record for page resource listing. */
    if (pdf_image_count == pdf_image_capacity) {
        int nc = pdf_image_capacity ? pdf_image_capacity * 2 : 8;
        PDF_Image* ni = (PDF_Image*)realloc(pdf_images,
                                            sizeof(PDF_Image) * (size_t)nc);
        if (!ni) return 0;
        pdf_images = ni;
        pdf_image_capacity = nc;
    }
    pdf_images[pdf_image_count].obj_id = img_id;
    pdf_images[pdf_image_count].width  = w;
    pdf_images[pdf_image_count].height = h;
    pdf_image_count++;

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return img_id;
}

static void pdf_draw_image(int obj_id, float x, float y,
                           float draw_w, float draw_h) {
    fprintf(content_fp,
        "q\n"
        "%.2f 0 0 %.2f %.2f %.2f cm\n"
        "/Im%d Do\n"
        "Q\n",
        draw_w, draw_h, x, y, obj_id);
}

static void end_document() {
    int max_object_id;
    long xref_start;

    pdf_finish_current_page();
    pdf_write_page_objects();
    pdf_write_pages_object();

    max_object_id = next_obj_id - 1;
    xref_start = ftell(pdf_fp);
    fprintf(pdf_fp, "xref\n");
    fprintf(pdf_fp, "0 %d\n", max_object_id + 1);

    fprintf(pdf_fp, "0000000000 65535 f \n");

    for (int i = 1; i <= max_object_id; i++) {
        fprintf(pdf_fp, "%010ld 00000 n \n", offsets[i]);
    }

    fprintf(pdf_fp,
        "trailer\n"
        "<< /Size %d /Root %d 0 R >>\n"
        "startxref\n%ld\n"
        "%%%%EOF\n",
        max_object_id + 1,
        PDF_CATALOG_ID,
        xref_start
    );

    fclose(pdf_fp);
    free(g_font_data);
    free(g_text_font_data);
    free(pages);
    free(offsets);
    free(pdf_images);

    pdf_fp = NULL;
    g_font_data = NULL;
    g_text_font_data = NULL;
    pages = NULL;
    offsets = NULL;
    pdf_images = NULL;
    page_count = 0;
    page_capacity = 0;
    offsets_capacity = 0;
    current_content_id = 0;
    next_obj_id = 0;
    pdf_image_count = 0;
    pdf_image_capacity = 0;
    content_fp = NULL;
    content_buf = NULL;
    content_buf_size = 0;
}

static void write_font_glyph(const char* font_name, float x, float y,
                             float size, uint16_t gid) {
    fprintf(content_fp,
        "BT\n"
        "/%s %.2f Tf\n"
        "1 0 0 1 %.2f %.2f Tm\n"
        "<%04X> Tj\n"
        "ET\n",
        font_name, size,
        x, y, gid);
}

static void write_glyph(float x, float y, uint16_t gid){
    write_font_glyph("F1", x, y, 40.0f, gid);
}

static void draw_glyph(float x, float y, glyph g) {
    uint16_t gid = ttf_glyph_index(&g_font, g);
    write_glyph(x, y, gid);
}

static float text_glyph_width(uint32_t codepoint, float size) {
    uint16_t gid = ttf_glyph_index(&g_text_font, codepoint);
    return font_glyph_width(&g_text_font, gid, size);
}

static void draw_text_glyph(float x, float y, float size, uint32_t codepoint) {
    uint16_t gid = ttf_glyph_index(&g_text_font, codepoint);
    if (!gid) {
        return;
    }

    write_font_glyph("F2", x, y, size, gid);
}

static float text_width(const char* text, float size) {
    float width = 0.0f;

    while (text && *text) {
        uint32_t codepoint = utf8_next_codepoint(&text);
        width += text_glyph_width(codepoint, size);
    }

    return width;
}

static void draw_text(float x, float y, float size, const char* text) {
    while (text && *text) {
        uint32_t codepoint = utf8_next_codepoint(&text);
        uint16_t gid = ttf_glyph_index(&g_text_font, codepoint);

        if (gid) {
            write_font_glyph("F2", x, y, size, gid);
            x += font_glyph_width(&g_text_font, gid, size);
        }
    }
}