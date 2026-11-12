#!/usr/bin/env python3
"""Render the canonical submission manuscript to an editable academic DOCX.

Requires python-docx. Equations use native OMML; no image or external service.
The canonical Markdown remains the word-count and content source of truth.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

from docx import Document
from docx.enum.table import WD_CELL_VERTICAL_ALIGNMENT, WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Inches, Pt, RGBColor
from docx.opc.constants import RELATIONSHIP_TYPE


def element(name, **attributes):
    result = OxmlElement(name)
    for key, value in attributes.items():
        result.set(qn(key), str(value))
    return result


def math_run(text, plain=False):
    run = element('m:r')
    props = element('m:rPr')
    props.append(element('m:sty', **{'m:val': 'p' if plain else 'i'}))
    run.append(props)
    word_props = element('w:rPr')
    word_props.append(element('w:rFonts', **{'w:ascii': 'Cambria Math', 'w:hAnsi': 'Cambria Math'}))
    word_props.append(element('w:sz', **{'w:val': '24'}))
    run.append(word_props)
    node = element('m:t')
    node.set(qn('xml:space'), 'preserve')
    node.text = text
    run.append(node)
    return run


def subscript(base, sub):
    node = element('m:sSub')
    main = element('m:e')
    main.append(math_run(base))
    tail = element('m:sub')
    tail.append(math_run(sub, plain=True))
    node.extend([main, tail])
    return node


def math_paragraph(document, nodes):
    paragraph = document.add_paragraph()
    paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
    paragraph.paragraph_format.space_before = Pt(2)
    paragraph.paragraph_format.space_after = Pt(7)
    paragraph.paragraph_format.keep_together = True
    root = element('m:oMathPara')
    props = element('m:oMathParaPr')
    props.append(element('m:jc', **{'m:val': 'center'}))
    root.append(props)
    math = element('m:oMath')
    math.extend(nodes)
    root.append(math)
    paragraph._p.append(root)


def add_equation(document, latex, index):
    if index == 0:
        assert '\\mathcal{O}(G,q,R)' in latex and 'i=1' in latex
        sequence = element('m:sSubSup')
        main, lower, upper = element('m:e'), element('m:sub'), element('m:sup')
        main.extend([math_run('[('), subscript('r', 'i'), math_run(', '),
                     subscript('p', 'i'), math_run(', '), subscript('b', 'i'), math_run(')]')])
        lower.append(math_run('i = 1'))
        upper.append(math_run('m'))
        sequence.extend([main, lower, upper])
        math_paragraph(document, [math_run('O(G, q, R) = '), sequence, math_run('.')])
    elif index == 1:
        assert '\\operatorname{success}(W_T)' in latex and 'D_T(W_T)' in latex
        math_paragraph(document, [math_run('success(', plain=True), subscript('W', 'T'),
            math_run(') ⇒ '), subscript('D', 'T'), math_run('('), subscript('W', 'T'),
            math_run(') = O(G, q, R).')])
    elif index == 2:
        assert 'T_{\\mathrm{service}}' in latex and 'timed\\ audits' in latex
        math_paragraph(document, [subscript('T', 'service'), math_run(' = '),
            subscript('T', 'spawn through close'), math_run(' − '), subscript('T', 'timed audits'), math_run('.')])
    else:
        raise ValueError('Unrecognized equation: preserve canonical semantics explicitly')


def add_link(paragraph, url, label=None):
    relation = paragraph.part.relate_to(url, RELATIONSHIP_TYPE.HYPERLINK, is_external=True)
    link = element('w:hyperlink', **{'r:id': relation})
    run = element('w:r')
    props = element('w:rPr')
    props.append(element('w:color', **{'w:val': '000000'}))
    props.append(element('w:u', **{'w:val': 'single'}))
    run.append(props)
    text = element('w:t')
    text.text = label or url
    run.append(text)
    link.append(run)
    paragraph._p.append(link)


def add_inline(paragraph, text):
    pattern = r'(https?://\S+|\*\*[^*]+\*\*|\*[^*]+\*|`[^`]+`)'
    for item in re.split(pattern, text):
        if not item:
            continue
        if item.startswith(('https://', 'http://')):
            add_link(paragraph, item)
        elif item.startswith('**') and item.endswith('**'):
            paragraph.add_run(item[2:-2]).bold = True
        elif item.startswith('*') and item.endswith('*'):
            paragraph.add_run(item[1:-1]).italic = True
        elif item.startswith('`') and item.endswith('`'):
            run = paragraph.add_run(item[1:-1])
            run.font.name = 'Consolas'
            run.font.size = Pt(10)
        else:
            paragraph.add_run(item)


def add_table(document, rows):
    rows = [[cell.strip() for cell in line.strip().strip('|').split('|')] for line in rows]
    rows = [row for row in rows if not all(re.fullmatch(r':?-+:?', c) for c in row)]
    count = len(rows[0])
    table = document.add_table(rows=1, cols=count)
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    table.autofit = False
    widths = {3: [1.05, 0.75, 5.0], 5: [1.00, 1.25, 1.25, 1.15, 2.15]}[count]
    properties = table._tbl.tblPr
    borders = element('w:tblBorders')
    for edge in ['top', 'left', 'bottom', 'right', 'insideH', 'insideV']:
        borders.append(element('w:'+edge, **{'w:val':'single','w:sz':'4','w:color':'D9D9D9'}))
    properties.append(borders)
    for col, width in zip(table.columns, widths):
        col.width = Inches(width)
    for index, values in enumerate(rows):
        row = table.rows[0] if index == 0 else table.add_row()
        if index == 0:
            row._tr.get_or_add_trPr().append(element('w:tblHeader', **{'w:val':'true'}))
        row._tr.get_or_add_trPr().append(element('w:cantSplit'))
        for column, (cell, value) in enumerate(zip(row.cells, values)):
            cell.width = Inches(widths[column])
            cell.vertical_alignment = WD_CELL_VERTICAL_ALIGNMENT.CENTER
            cell_props = cell._tc.get_or_add_tcPr()
            margin = element('w:tcMar')
            for side in ['top','bottom','left','right']:
                margin.append(element('w:'+side, **{'w:w':'65' if side in ('top','bottom') else '85','w:type':'dxa'}))
            cell_props.append(margin)
            if index == 0:
                cell_props.append(element('w:shd', **{'w:fill':'E8EDF2'}))
            paragraph = cell.paragraphs[0]
            paragraph.alignment = WD_ALIGN_PARAGRAPH.LEFT if column == 0 or (count == 3 and column == 2) else WD_ALIGN_PARAGRAPH.CENTER
            paragraph.paragraph_format.space_after = Pt(1)
            paragraph.paragraph_format.space_before = Pt(1)
            paragraph.paragraph_format.line_spacing = 1.04
            paragraph.paragraph_format.keep_with_next = index < len(rows)-1
            run = paragraph.add_run(value)
            run.font.size = Pt(10.5)
            run.bold = index == 0
    blank = document.add_paragraph()
    blank.paragraph_format.space_after = Pt(1)
    blank.paragraph_format.space_before = Pt(0)
    blank.paragraph_format.line_spacing = 0.4
    blank.add_run().font.size = Pt(3)


def build(source, target, receipt):
    text = source.read_text(encoding='utf-8')
    doc = Document()
    section = doc.sections[0]
    section.page_width, section.page_height = Inches(8.5), Inches(11)
    section.top_margin = section.bottom_margin = Inches(0.70)
    section.left_margin = section.right_margin = Inches(0.75)
    section.footer_distance = Inches(0.30)
    styles = doc.styles
    for border in list(styles._element.xpath('.//w:pBdr')):
        border.getparent().remove(border)
    for name in ['Normal','Title','Subtitle','Heading 1','Heading 2','Heading 3','Footer']:
        styles[name].font.name = 'Times New Roman'
        styles[name].font.color.rgb = RGBColor(0,0,0)
        styles[name]._element.get_or_add_rPr().rFonts.set(qn('w:ascii'), 'Times New Roman')
        styles[name]._element.rPr.rFonts.set(qn('w:hAnsi'), 'Times New Roman')
        for attribute in list(styles[name]._element.rPr.rFonts.attrib):
            if 'theme' in attribute.lower():
                del styles[name]._element.rPr.rFonts.attrib[attribute]
    styles['Normal'].font.size = Pt(11)
    styles['Normal'].paragraph_format.line_spacing = 1.00
    styles['Normal'].paragraph_format.space_after = Pt(4)
    styles['Normal'].paragraph_format.widow_control = True
    styles['Title'].font.size = Pt(20)
    styles['Title'].font.bold = True
    styles['Title'].paragraph_format.space_after = Pt(5)
    styles['Subtitle'].font.size = Pt(11)
    styles['Subtitle'].font.italic = True
    styles['Subtitle'].paragraph_format.space_after = Pt(13)
    styles['Heading 1'].font.size = Pt(13)
    styles['Heading 1'].font.bold = True
    styles['Heading 1'].paragraph_format.space_before = Pt(11)
    styles['Heading 1'].paragraph_format.space_after = Pt(4)
    styles['Heading 2'].font.size = Pt(11.5)
    styles['Heading 2'].font.bold = True
    styles['Heading 2'].paragraph_format.space_before = Pt(7)
    styles['Heading 2'].paragraph_format.space_after = Pt(3)
    for name in ['Title','Subtitle','Heading 1','Heading 2']:
        styles[name].paragraph_format.keep_with_next = True
    footer = section.footer.paragraphs[0]
    footer.alignment = WD_ALIGN_PARAGRAPH.CENTER
    footer.style = styles['Footer']
    footer.paragraph_format.space_after = Pt(0)
    run = footer.add_run()
    run.font.size = Pt(9)
    run._r.append(element('w:fldChar', **{'w:fldCharType':'begin'}))
    instruction = element('w:instrText')
    instruction.text = ' PAGE '
    run._r.append(instruction)
    run._r.append(element('w:fldChar', **{'w:fldCharType':'end'}))
    doc.core_properties.title = 'Suffice Exact Repository Observations for Developer Agents'
    doc.core_properties.subject = 'Generation identity retained state and complete delivery'
    doc.core_properties.author = ''
    doc.core_properties.last_modified_by = ''
    doc.core_properties.keywords = 'developer agents; repository observations; generation identity; transport; reproducibility'
    doc.core_properties.comments = ''
    lines = text.splitlines()
    i, equation_count = 0, 0
    references = False
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            i += 1
            continue
        if line.startswith('# '):
            title = line[2:].replace(' for Developer Agents', '\nfor Developer Agents')
            doc.add_paragraph(title, style='Title')
            i += 1
        elif i < 5 and line.startswith('*') and line.endswith('*'):
            doc.add_paragraph(line[1:-1], style='Subtitle')
            i += 1
        elif line.startswith('## '):
            heading = line[3:]
            doc.add_paragraph(heading, style='Heading 1')
            references = heading == 'References'
            i += 1
        elif line.startswith('### '):
            heading = re.sub(r'^\d+\.\d+\s+', '', line[4:])
            doc.add_paragraph(heading, style='Heading 2')
            i += 1
        elif line == '$$':
            end = i + 1
            while lines[end].strip() != '$$':
                end += 1
            add_equation(doc, '\n'.join(lines[i+1:end]), equation_count)
            equation_count += 1
            i = end + 1
        elif line.startswith('|'):
            block = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                block.append(lines[i])
                i += 1
            add_table(doc, block)
        elif line.startswith('!['):
            match = re.fullmatch(r'!\[([^]]+)\]\(([^)]+)\)', line)
            if not match:
                raise ValueError('Invalid figure marker')
            paragraph = doc.add_paragraph()
            paragraph.paragraph_format.keep_with_next = True
            paragraph.paragraph_format.space_before = Pt(4)
            paragraph.paragraph_format.space_after = Pt(2)
            paragraph.alignment = WD_ALIGN_PARAGRAPH.CENTER
            paragraph.add_run().add_picture(str(source.parent / match.group(2)), width=Inches(6.65))
            i += 1
        else:
            body = [line]
            i += 1
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(('#','|','$$')):
                body.append(lines[i].strip())
                i += 1
            paragraph = doc.add_paragraph()
            add_inline(paragraph, ' '.join(body))
            if line.startswith(('*Figure ', '*Table ')):
                paragraph.paragraph_format.keep_together = True
                paragraph.paragraph_format.space_after = Pt(4)
                if line.startswith('*Table '):
                    paragraph.paragraph_format.keep_with_next = True
                for run in paragraph.runs:
                    run.font.size = Pt(9.5)
            if references:
                paragraph.paragraph_format.space_after = Pt(4)
                paragraph.paragraph_format.keep_together = True
                for run_xml in paragraph._p.xpath('.//w:r'):
                    props = run_xml.get_or_add_rPr()
                    for size in list(props.findall(qn('w:sz'))):
                        props.remove(size)
                    props.append(element('w:sz', **{'w:val': '19'}))
    assert equation_count == 3
    target.parent.mkdir(parents=True, exist_ok=True)
    doc.save(target)
    xml = doc._element.xml
    assert xml.count('<m:oMath>') == 3
    assert '<m:sSubSup>' in xml and '⇒' in xml
    receipt.write_text(json.dumps({
        'status':'created', 'source_file':source.name,
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'docx_file':target.name,'docx_sha256':hashlib.sha256(target.read_bytes()).hexdigest(),
        'native_math_displays':3,'canonical_math_blocks':equation_count,
        'tables':len(doc.tables), 'visual_review':'pending',
        'byline':'omitted; Kaggle account attribution and author declarations require owner review'
    },indent=2)+'\n', encoding='utf-8')
    print(target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--receipt',type=Path,required=True)
    args = parser.parse_args()
    build(args.source,args.output,args.receipt)
