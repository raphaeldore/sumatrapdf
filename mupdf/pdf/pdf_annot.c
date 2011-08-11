#include "fitz.h"
#include "mupdf.h"

void
pdf_free_link(pdf_link *link)
{
	if (link->next)
		pdf_free_link(link->next);
	if (link->dest)
		fz_drop_obj(link->dest);
	fz_free(link);
}

static fz_obj *
resolve_dest(pdf_xref *xref, fz_obj *dest)
{
	if (fz_is_name(dest) || fz_is_string(dest))
	{
		dest = pdf_lookup_dest(xref, dest);
		return resolve_dest(xref, dest);
	}

	else if (fz_is_array(dest))
	{
		return dest;
	}

	else if (fz_is_dict(dest))
	{
		dest = fz_dict_gets(dest, "D");
		return resolve_dest(xref, dest);
	}

	else if (fz_is_indirect(dest))
		return dest;

	return NULL;
}

pdf_link *
pdf_load_link(pdf_xref *xref, fz_obj *dict)
{
	fz_obj *dest;
	fz_obj *action;
	fz_obj *obj;
	fz_rect bbox;
	pdf_link_kind kind;

	dest = NULL;

	obj = fz_dict_gets(dict, "Rect");
	if (obj)
		bbox = pdf_to_rect(obj);
	else
		bbox = fz_empty_rect;

	obj = fz_dict_gets(dict, "Dest");
	if (obj)
	{
		kind = PDF_LINK_GOTO;
		dest = resolve_dest(xref, obj);
	}

	action = fz_dict_gets(dict, "A");

	/* fall back to additional action button's down/up action */
	if (!action)
		action = fz_dict_getsa(fz_dict_gets(dict, "AA"), "U", "D");

	if (action)
	{
		obj = fz_dict_gets(action, "S");
		if (fz_is_name(obj) && !strcmp(fz_to_name(obj), "GoTo"))
		{
			kind = PDF_LINK_GOTO;
			dest = resolve_dest(xref, fz_dict_gets(action, "D"));
		}
		else if (fz_is_name(obj) && !strcmp(fz_to_name(obj), "URI"))
		{
			kind = PDF_LINK_URI;
			dest = fz_dict_gets(action, "URI");
		}
		else if (fz_is_name(obj) && !strcmp(fz_to_name(obj), "Launch"))
		{
			kind = PDF_LINK_LAUNCH;
			dest = fz_dict_gets(action, "F");
		}
		else if (fz_is_name(obj) && !strcmp(fz_to_name(obj), "Named"))
		{
			kind = PDF_LINK_NAMED;
			dest = fz_dict_gets(action, "N");
		}
		else if (fz_is_name(obj) && (!strcmp(fz_to_name(obj), "GoToR")))
		{
			kind = PDF_LINK_ACTION;
			dest = action;
		}
		else
		{
			dest = NULL;
		}
	}

	if (dest)
	{
		pdf_link *link = fz_malloc(sizeof(pdf_link));
		link->kind = kind;
		link->rect = bbox;
		link->dest = fz_keep_obj(dest);
		link->next = NULL;
		return link;
	}

	return NULL;
}

void
pdf_load_links(pdf_link **linkp, pdf_xref *xref, fz_obj *annots)
{
	pdf_link *link, *head, *tail;
	fz_obj *obj;
	int i;

	head = tail = NULL;
	link = NULL;

	for (i = 0; i < fz_array_len(annots); i++)
	{
		obj = fz_array_get(annots, i);
		link = pdf_load_link(xref, obj);
		if (link)
		{
			if (!head)
				head = tail = link;
			else
			{
				tail->next = link;
				tail = link;
			}
		}
	}

	*linkp = head;
}

void
pdf_free_annot(pdf_annot *annot)
{
	if (annot->next)
		pdf_free_annot(annot->next);
	if (annot->ap)
		pdf_drop_xobject(annot->ap);
	if (annot->obj)
		fz_drop_obj(annot->obj);
	fz_free(annot);
}

static void
pdf_transform_annot(pdf_annot *annot)
{
	fz_matrix matrix = annot->ap->matrix;
	fz_rect bbox = annot->ap->bbox;
	fz_rect rect = annot->rect;
	float w, h, x, y;

	bbox = fz_transform_rect(matrix, bbox);
	w = (rect.x1 - rect.x0) / (bbox.x1 - bbox.x0);
	h = (rect.y1 - rect.y0) / (bbox.y1 - bbox.y0);
	x = rect.x0 - bbox.x0;
	y = rect.y0 - bbox.y0;
	annot->matrix = fz_concat(fz_scale(w, h), fz_translate(x, y));
}

/* SumatraPDF: synthesize appearance streams for a few more annotations */
static pdf_annot *
pdf_create_annot(fz_rect rect, fz_obj *base_obj, fz_buffer *content, fz_obj *resources)
{
	pdf_annot *annot;
	pdf_xobject *form;

	form = fz_malloc(sizeof(pdf_xobject));
	memset(form, 0, sizeof(pdf_xobject));
	form->refs = 1;
	form->matrix = fz_identity;
	form->bbox.x1 = rect.x1 - rect.x0;
	form->bbox.y1 = rect.y1 - rect.y0;
	form->isolated = 1;
	form->contents = content;
	form->resources = resources;

	annot = fz_malloc(sizeof(pdf_annot));
	annot->obj = base_obj;
	annot->rect = rect;
	annot->ap = form;
	annot->next = NULL;

	pdf_transform_annot(annot);

	return annot;
}

static fz_obj *
pdf_clone_for_view_only(pdf_xref *xref, fz_obj *obj)
{
	char *string = "<< /OCGs << /Usage << /Print << /PrintState /OFF >> /Export << /ExportState /OFF >> >> >> >>";
	fz_stream *stream = fz_open_memory(string, strlen(string));
	fz_obj *tmp = NULL;
	pdf_parse_stm_obj(&tmp, NULL, stream, xref->scratch, sizeof(xref->scratch));
	fz_close(stream);

	obj = fz_copy_dict(pdf_resolve_indirect(obj));
	fz_dict_puts(obj, "OC", tmp);
	fz_drop_obj(tmp);

	return obj;
}

static void
fz_buffer_printf(fz_buffer *buffer, char *fmt, ...)
{
	int count;
	va_list args;
	va_start(args, fmt);
retry_larger_buffer:
	count = _vsnprintf(buffer->data + buffer->len, buffer->cap - buffer->len, fmt, args);
	if (count < 0 || count >= buffer->cap - buffer->len)
	{
		fz_grow_buffer(buffer);
		goto retry_larger_buffer;
	}
	buffer->len += count;
	va_end(args);
}

/* SumatraPDF: partial support for link borders */
static pdf_annot *
pdf_create_link_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *border, *color, *dashes;
	fz_rect rect;
	fz_buffer *content;
	int i;

	border = fz_dict_gets(obj, "Border");
	if (fz_to_real(fz_array_get(border, 2)) <= 0)
		return NULL;

	color = fz_dict_gets(obj, "C");
	dashes = fz_array_get(border, 3);
	rect = pdf_to_rect(fz_dict_gets(obj, "Rect"));

	obj = pdf_clone_for_view_only(xref, obj);

	// TODO: draw rounded rectangles if the first two /Border values are non-zero
	content = fz_new_buffer(128);
	fz_buffer_printf(content, "q %.4f w [", fz_to_real(fz_array_get(border, 2)));
	for (i = 0; i < fz_array_len(dashes); i++)
		fz_buffer_printf(content, "%.4f ", fz_to_real(fz_array_get(dashes, i)));
	fz_buffer_printf(content, "] 0 d %.4f %.4f %.4f RG 0 0 %.4f %.4f re S Q",
		fz_to_real(fz_array_get(color, 0)), fz_to_real(fz_array_get(color, 1)),
		fz_to_real(fz_array_get(color, 2)), rect.x1 - rect.x0, rect.y1 - rect.y0);

	return pdf_create_annot(rect, obj, content, NULL);
}

// content stream adapted from Poppler's Annot.cc, licensed under GPLv2 and later
#define ANNOT_TEXT_AP_COMMENT \
	"0.533333 0.541176 0.521569 RG 2 w\n"                                         \
	"0 J 1 j [] 0 d\n"                                                            \
	"4 M 8 20 m 16 20 l 18.363 20 20 18.215 20 16 c 20 13 l 20 10.785 18.363 9\n" \
	"16 9 c 13 9 l 8 3 l 8 9 l 8 9 l 5.637 9 4 10.785 4 13 c 4 16 l 4 18.215\n"   \
	"5.637 20 8 20 c h\n"                                                         \
	"8 20 m S\n"                                                                  \
	"0.729412 0.741176 0.713725 RG 8 21 m 16 21 l 18.363 21 20 19.215 20 17\n"    \
	"c 20 14 l 20 11.785 18.363 10\n"                                             \
	"16 10 c 13 10 l 8 4 l 8 10 l 8 10 l 5.637 10 4 11.785 4 14 c 4 17 l 4\n"     \
	"19.215 5.637 21 8 21 c h\n"                                                  \
	"8 21 m S\n"

/* SumatraPDF: partial support for text icons */
static pdf_annot *
pdf_create_text_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_buffer *content = fz_new_buffer(512);
	fz_rect rect = pdf_to_rect(fz_dict_gets(obj, "Rect"));
	rect.x1 = rect.x0 + 24;
	rect.y1 = rect.y0 + 24;

	obj = pdf_clone_for_view_only(xref, obj);
	// TODO: support other icons by /Name: Note, Key, Help, Paragraph, NewParagraph, Insert
	// TODO: make icon semi-transparent(?)
	fz_buffer_printf(content, "q %s Q", ANNOT_TEXT_AP_COMMENT);

	return pdf_create_annot(rect, obj, content, NULL);
}

/* cf. http://bugs.ghostscript.com/show_bug.cgi?id=692078 */
static fz_obj *
pdf_dict_get_inheritable(pdf_xref *xref, fz_obj *obj, char *key)
{
	while (obj)
	{
		fz_obj *val = fz_dict_gets(obj, key);
		if (val)
			return val;
		obj = fz_dict_gets(obj, "Parent");
	}
	return fz_dict_gets(fz_dict_gets(fz_dict_gets(xref->trailer, "Root"), "AcroForm"), key);
}

static float
pdf_extract_font_size(pdf_xref *xref, char *appearance, char **fontName)
{
	fz_stream *stream = fz_open_memory(appearance, strlen(appearance));
	int tok, len;
	float fontSize = 0;
	*fontName = NULL;
	do
	{
		fz_error error = pdf_lex(&tok, stream, xref->scratch, sizeof(xref->scratch), &len);
		if (error || tok == PDF_TOK_EOF)
		{
			fz_free(*fontName);
			*fontName = NULL;
			break;
		}
		if (tok == PDF_TOK_NAME)
		{
			fz_free(*fontName);
			*fontName = fz_strdup(xref->scratch);
		}
		else if (tok == PDF_TOK_REAL || tok == PDF_TOK_INT)
			fontSize = fz_atof(xref->scratch);
	} while (tok != PDF_TOK_KEYWORD || strcmp(xref->scratch, "Tf") != 0);
	fz_close(stream);
	return fontSize;
}

static fz_obj *
pdf_get_ap_stream(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *ap = fz_dict_gets(obj, "AP");
	if (!fz_is_dict(ap))
		return NULL;

	ap = fz_dict_gets(ap, "N");
	if (!pdf_is_stream(xref, fz_to_num(ap), fz_to_gen(ap)))
		ap = fz_dict_get(ap, fz_dict_gets(obj, "AS"));
	if (!pdf_is_stream(xref, fz_to_num(ap), fz_to_gen(ap)))
		return NULL;

	return ap;
}

static void
pdf_prepend_ap_background(fz_buffer *content, pdf_xref *xref, fz_obj *obj)
{
	pdf_xobject *form;
	int i;
	fz_obj *ap = pdf_get_ap_stream(xref, obj);
	if (!ap)
		return;
	if (pdf_load_xobject(&form, xref, ap) != fz_okay)
		return;

	for (i = 0; i < form->contents->len - 3 && memcmp(form->contents->data + i, "/Tx", 3) != 0; i++);
	if (i == form->contents->len - 3)
		i = form->contents->len;
	if (content->cap < content->len + i)
		fz_resize_buffer(content, content->len + i);
	memcpy(content->data + content->len, form->contents->data, i);
	content->len += i;

	pdf_drop_xobject(form);
}

static void
pdf_string_to_Tj(fz_buffer *content, fz_obj *value, int is_password)
{
	unsigned short *ucs2 = pdf_to_ucs2(value);
	unsigned short *uc;
	fz_buffer_printf(content, " (");
	for (uc = ucs2; *uc; uc++)
	{
		if (is_password)
			*uc = '*';
		else if (*uc > 0xFF)
			*uc = '?';
		// TODO: convert to CID
		if (*uc < 0x20 || *uc == '(' || *uc == ')' || *uc == '\\')
			fz_buffer_printf(content, "\\%03o", *uc);
		else
			fz_buffer_printf(content, "%c", *uc);
	}
	fz_buffer_printf(content, ") Tj");
	fz_free(ucs2);
}

static pdf_annot *
pdf_update_tx_widget_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *ap, *res, *value;
	fz_rect rect;
	fz_buffer *content;
	int flags, align;
	float fontSize;
	char *fontName;
	int tmpLen;

	if (strcmp(fz_to_name(fz_dict_gets(obj, "Subtype")), "Widget") != 0)
		return NULL;
	if (!fz_to_bool(pdf_dict_get_inheritable(xref, NULL, "NeedAppearances")) && pdf_get_ap_stream(xref, obj))
		return NULL;
	value = pdf_dict_get_inheritable(xref, obj, "FT");
	if (strcmp(fz_to_name(value), "Tx") != 0)
		return NULL;

	ap = pdf_dict_get_inheritable(xref, obj, "DA");
	value = pdf_dict_get_inheritable(xref, obj, "V");
	if (!ap || !value)
		return NULL;

	res = pdf_dict_get_inheritable(xref, obj, "DR");
	rect = pdf_to_rect(fz_dict_gets(obj, "Rect"));

	flags = fz_to_int(fz_dict_gets(obj, "Ff"));
	if (flags & ((1 << 12) /* multiline */ | (1 << 24) /* comb */ | (1 << 25) /* richtext */))
		fz_warn("missing support for multiline, combed and richtext fields");

	fontSize = pdf_extract_font_size(xref, fz_to_str_buf(ap), &fontName);
	if (!fontSize || !fontName)
		fontSize = floor(rect.y1 - rect.y0 - 2);

	content = fz_new_buffer(256);
	pdf_prepend_ap_background(content, xref, obj);
	fz_buffer_printf(content, "/Tx BMC q ");
	fz_buffer_printf(content, "1 1 %.4f %.4f re W n", rect.x1 - rect.x0 - 2.0f, rect.y1 - rect.y0 - 2.0f);
	fz_buffer_printf(content, " BT ");
	fz_buffer_printf(content, "%s ", fz_to_str_buf(ap));
	if (fontName)
		fz_buffer_printf(content, "/%s %.4f Tf ", fontName, fontSize);
	fz_free(fontName);
	tmpLen = content->len;
	fz_buffer_printf(content, "1 0 0 1 %.4f %.4f Tm", 2.0f, 0.5f * (rect.y1 - rect.y0) - 0.4f * fontSize);
	pdf_string_to_Tj(content, value, (flags & (1 << 13) /* password */));
	fz_buffer_printf(content, " ET Q EMC");

	align = fz_to_int(fz_dict_gets(obj, "Q"));
	if (align != 0)
	{
		fz_bbox bbox;
		fz_device *dev = fz_new_bbox_device(&bbox);
		fz_error error = pdf_run_glyph(xref, res, content, dev, fz_identity);
		if (!error)
		{
			float x = rect.x1 - rect.x0 - 4.0f - (bbox.x1 - bbox.x0);
			if (align == 1 /* centered */)
				x *= 0.5f;
			content->len = tmpLen;
			fz_buffer_printf(content, "1 0 0 1 %.4f %.4f Tm", 2.0f + x, 0.5f * (rect.y1 - rect.y0) - 0.4f * fontSize);
			pdf_string_to_Tj(content, value, (flags & (1 << 13) /* password */));
			fz_buffer_printf(content, " ET Q EMC");
		}
		fz_free_device(dev);
	}

	return pdf_create_annot(rect, fz_keep_obj(obj), content, res ? fz_keep_obj(res) : NULL);
}

static pdf_annot *
pdf_create_annot_with_appearance(pdf_xref *xref, fz_obj *obj)
{
	if (!strcmp(fz_to_name(fz_dict_gets(obj, "Subtype")), "Link"))
		return pdf_create_link_annot(xref, obj);
	if (!strcmp(fz_to_name(fz_dict_gets(obj, "Subtype")), "Text"))
		return pdf_create_text_annot(xref, obj);
	return NULL;
}

void
pdf_load_annots(pdf_annot **annotp, pdf_xref *xref, fz_obj *annots)
{
	pdf_annot *annot, *head, *tail;
	fz_obj *obj, *ap, *as, *n, *rect;
	pdf_xobject *form;
	fz_error error;
	int i;

	head = tail = NULL;
	annot = NULL;

	for (i = 0; i < fz_array_len(annots); i++)
	{
		obj = fz_array_get(annots, i);

		/* cf. http://bugs.ghostscript.com/show_bug.cgi?id=692078 */
		if ((annot = pdf_update_tx_widget_annot(xref, obj)))
		{
			if (!head)
				head = tail = annot;
			else
			{
				tail->next = annot;
				tail = annot;
			}
			continue;
		}

		rect = fz_dict_gets(obj, "Rect");
		ap = fz_dict_gets(obj, "AP");
		as = fz_dict_gets(obj, "AS");
		if (fz_is_dict(ap))
		{
			n = fz_dict_gets(ap, "N"); /* normal state */

			/* lookup current state in sub-dictionary */
			if (!pdf_is_stream(xref, fz_to_num(n), fz_to_gen(n)))
				n = fz_dict_get(n, as);

			if (pdf_is_stream(xref, fz_to_num(n), fz_to_gen(n)))
			{
				error = pdf_load_xobject(&form, xref, n);
				if (error)
				{
					fz_catch(error, "ignoring broken annotation");
					continue;
				}

				annot = fz_malloc(sizeof(pdf_annot));
				annot->obj = fz_keep_obj(obj);
				annot->rect = pdf_to_rect(rect);
				annot->ap = form;
				annot->next = NULL;

				pdf_transform_annot(annot);

				if (annot)
				{
					if (!head)
						head = tail = annot;
					else
					{
						tail->next = annot;
						tail = annot;
					}
				}
			}
		}
		/* SumatraPDF: synthesize appearance streams for a few more annotations */
		else if ((annot = pdf_create_annot_with_appearance(xref, obj)))
		{
			if (!head)
				head = tail = annot;
			else
			{
				tail->next = annot;
				tail = annot;
			}
		}
	}

	*annotp = head;
}
