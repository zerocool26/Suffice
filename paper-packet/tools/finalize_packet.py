#!/usr/bin/env python3
"""Validate and package the revised manuscript, preserved evidence and read bridge."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile
from pypdf import PdfReader

EXPECTED_PAPER = '8596948fc0305b0acaeb147cd601ecdbc9c88935f8b54456eec180a8a5277b19'
EXPECTED_SOURCE = '1cda9360c3d0d013309236a0720d13b7c3f01735a590761894a8bb68adb77205'
EXPECTED_PREVIOUS = 'bfd32da30fe9ed6ee86c199e4cc453985bd5e7a9e33db3b08259d4b55d474e53'
FIGURE_TEXT = ('Paired block Geometric mean Median Reuse Pressure Mixed Computed Empty '
               'JSONL binary service time above 1 favors binary 1.0 1.5 2.0 2.5')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text())


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n')


def normalize(value):
    return re.sub(r'[^a-z0-9]', '', value.lower())


def build(work, source_archive, previous_archive, destination):
    manuscript = work/'paper/Suffice_Kaggle_Writeup.md'
    text = manuscript.read_text()
    assert digest(manuscript) == EXPECTED_PAPER
    assert digest(source_archive) == EXPECTED_SOURCE
    assert digest(previous_archive) == EXPECTED_PREVIOUS
    review = load(work/'research/revision_review.json')
    assert review['reviewed_manuscript_sha256'] == EXPECTED_PAPER
    assert not review['unresolved_required_corrections']
    visual = load(work/'paper/VISUAL_REVIEW.json')
    assert visual['status'] == 'passed'
    assert visual['pdf_sha256'] == digest(work/'paper/Suffice_Paper.pdf')
    reader = PdfReader(work/'paper/Suffice_Paper.pdf')
    assert len(reader.pages) == visual['pages'] == 5
    pdf_text = '\n'.join(re.sub(r'^\s*\d+\s*\n', '', p.extract_text()) for p in reader.pages)
    assert '\ufffd' not in pdf_text
    normalized_pdf = normalize(pdf_text)
    paragraphs = []
    for paragraph in re.split(r'\n\s*\n', text):
        if paragraph.startswith(('$$','|','![')):
            continue
        paragraph = paragraph.strip('*# \n')
        if len(paragraph.split()) >= 8:
            paragraphs.append(paragraph)
    missing = [p[:100] for p in paragraphs if normalize(p) not in normalized_pdf]
    assert not missing, missing
    headings = re.findall(r'^## (.+)$', text, re.M)
    for required in ['Abstract','1 Introduction','4 Experimental method','5 Results',
                     '7 Related work and limits','References']:
        assert required in headings
    assert set(re.findall(r'^\[(\d+)\]', text, re.M)) == set('1234567')
    for n in range(1,8):
        assert f'[{n}]' in text.split('## References')[0]
    assert text.count('\n$$\n') == 6
    assert text.count('![Paired transport observations]') == 1
    assert not re.search(r'\b(?:TODO|TBD|PLACEHOLDER)\b', text)
    segments = len(re.findall(r'[A-Za-z0-9]+',text))
    figure_segments = len(re.findall(r'[A-Za-z0-9]+',FIGURE_TEXT))
    counts = {'markdown_whitespace_words':len(text.split()),
              'markdown_ascii_alphanumeric_segments':segments,
              'additional_figure_label_segments':figure_segments,
              'conservative_total_including_figure_labels':segments+figure_segments,
              'pdf_extracted_whitespace_words_without_page_numbers':len(pdf_text.split()),
              'limit':3000,
              'scope':'All manuscript text including title, subtitle, references, tables, captions, math source and URLs; figure labels added separately. No exclusions claimed.',
              'editor_count':'Final Kaggle editor count remains an entrant check after any edits.'}
    assert counts['conservative_total_including_figure_labels'] < 3000
    summary_path = work.parent/'suffice_v0_13/results/transport_v0_13/SUMMARY.json'
    if summary_path.exists():
        summary = load(summary_path)
    else:
        with zipfile.ZipFile(source_archive) as source:
            summary = json.loads(source.read('Suffice_0_13_0/results/transport_v0_13/SUMMARY.json'))
    table_checks = []
    for key,label in [('reuse','Reuse'),('pressure','Pressure'),('mixed_queries','Mixed'),
                      ('computed','Computed'),('empty','Empty')]:
        paired = summary['scenarios'][key]['paired_comparisons']
        service = paired['jsonl_over_binary_service']
        raw = paired['jsonl_over_binary_raw_wall']['geometric_mean']
        reduction = paired['response_byte_reduction']['mean']*100
        expected = f"| {label} | {service['geometric_mean']:.3f} | {raw:.3f} | {service['pairs_above_one']}/10 | {reduction:.2f}% |"
        assert expected in text, expected
        table_checks.append({'scenario':key,'service_geometric_ratio':service['geometric_mean'],
                             'raw_geometric_ratio':raw,'mean_response_byte_reduction_percent':reduction,
                             'matches_published_rounding':True})
    bridge = load(work/'integration/validation_native/RESULT.json')
    assert bridge['status'] == 'passed' and bridge['inputs_stable']
    expected_bridge = {'native_processes':4,'checks':114,'complete_observations':12,
                       'verified_rows':30,'verified_body_bytes':2300,'expected_rejections':36,
                       'injected_prefix_failures':2,'identity_failures':2,'cross_transport_equalities':6}
    for k,v in expected_bridge.items():
        assert bridge['counters'][k] == v
    for name in ['repository_read_bridge.py','test_repository_read_bridge.py']:
        recorded = [v for k,v in bridge['inputs_after'].items() if Path(k).name==name]
        assert recorded == [digest(work/'integration'/name)]
    notebook = load(work/'verification/VERIFICATION_RECEIPT.json')
    assert notebook['status']=='passed' and notebook['code_cells_executed']==5
    assert notebook['notebook_sha256']==digest(work/'verification/Suffice_Offline_Verification.ipynb')
    assert notebook['counts']['complete_output_checks']==4840
    assert not any(notebook['external_action_audit'].values())
    closure = load(work/'verification/EXPORT_CLOSURE.json')
    assert closure['status']=='passed'
    assert closure['all_372_member_hashes_verified'] and closure['embedded_zip_matches_separate_zip']
    with zipfile.ZipFile(work/'paper/Suffice_Paper.docx') as z:
        document_xml=z.read('word/document.xml').decode()
        styles_xml=z.read('word/styles.xml').decode()
        assert document_xml.count('<m:oMath>')==3
        assert document_xml.count('<w:tbl>')==2
        assert '<w:pBdr>' not in styles_xml
    target=destination/'Suffice_Gemma4_Paper_Packet'
    target.mkdir(parents=True,exist_ok=False)
    for name in ['paper','verification','integration','research','tools']:
        shutil.copytree(work/name,target/name,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    for name in ['README.md','Submission_Checklist.md']:
        shutil.copyfile(work/name,target/name)
    support=target/'support'
    support.mkdir()
    shutil.copyfile(work/'Evidence_Map.md',support/'Evidence_Map.md')
    source=target/'source'
    source.mkdir()
    shutil.copyfile(source_archive,source/'Suffice_0_13_0.zip')
    with zipfile.ZipFile(source_archive) as z:
        (target/'integration/LICENSE').write_bytes(z.read('Suffice_0_13_0/LICENSE'))
    source_receipt={'source_release':source_archive.name,'source_sha256':EXPECTED_SOURCE,
                    'source_bytes':source_archive.stat().st_size,'source_members':721,
                    'previous_paper_packet_sha256':EXPECTED_PREVIOUS,
                    'source_and_previous_packet_unchanged':True,
                    'bridge_outside_frozen_measurement_source':True}
    save(source/'SOURCE_EXPORT_RECEIPT.json',source_receipt)
    (source/'README.md').write_text('# Preserved source release\n\nSuffice_0_13_0.zip is the unchanged full 0.13 release. Extract it for source, original inputs, frozen measurement source, process records, native build manifests and validation receipts. See its README.md and docs/transport_experiment_v0_13.md for fresh-build and rerun requirements. Existing MIT notices remain in place.\n\nThe new read bridge lives in the packet integration directory and is not part of the frozen transport timings. The packet verification notebook is a portable retained-evidence audit, not a native rebuild.\n')
    title=text.splitlines()[0][2:]
    subtitle=text.splitlines()[2].strip('*')
    abstract=text.split('## Abstract\n\n',1)[1].split('\n\n## ',1)[0]
    save(support/'Kaggle_Fields.json',{'title':title,'subtitle':subtitle,'abstract':abstract,
        'writeup_file':'paper/Suffice_Kaggle_Writeup.md','pdf_file':'paper/Suffice_Paper.pdf',
        'editable_source':'paper/Suffice_Paper.docx','notebook_file':'verification/Suffice_Offline_Verification.ipynb',
        'figure_file':'paper/figures/transport_paper.png','public_project_links':[],
        'public_pdf_url':None,'public_notebook_url':None,'submission_status':'not_submitted',
        'next_action':'Entrant verifies rights/account, uploads public companions if used, previews the Writeup and presses Submit.'})
    render_receipt=load(target/'paper/DOCX_RENDER_RECEIPT.json')
    render_receipt['visual_review']='passed; all five final pages inspected'
    render_receipt['pdf_sha256']=visual['pdf_sha256']
    save(target/'paper/DOCX_RENDER_RECEIPT.json',render_receipt)
    verification={'schema_version':2,'status':'passed','verified_utc':datetime.now(timezone.utc).isoformat(),
        'canonical_manuscript_sha256':EXPECTED_PAPER,'word_counts':counts,
        'pages':5,'tables':2,'figures':1,'native_equations':3,'references':7,
        'required_sections':headings,'pdf_text_paragraphs_checked':len(paragraphs),
        'pdf_missing_paragraphs':missing,'visual_review':visual,
        'table_arithmetic':table_checks,'bridge_gate':expected_bridge,
        'notebook_cells_passed':5,'notebook_sha256':notebook['notebook_sha256'],
        'no_model_inference':True,'no_publication_or_submission':True,
        'files':{f'paper/{p.name}':{'bytes':p.stat().st_size,'sha256':digest(p)}
                 for p in (target/'paper').iterdir() if p.is_file()}}
    save(support/'PAPER_VERIFICATION.json',verification)
    for name,descriptor in verification['files'].items():
        assert digest(target/name)==descriptor['sha256']
    paths=sorted(p for p in target.rglob('*') if p.is_file())
    manifest={'schema_version':2,'artifact':'Suffice paper revision based on 0.13 evidence',
              'status':'verified_local_packet_not_submitted',
              'files':[{'path':p.relative_to(target).as_posix(),'bytes':p.stat().st_size,'sha256':digest(p)} for p in paths]}
    save(target/'MANIFEST.json',manifest)
    archive=destination/'Suffice_Gemma4_Paper_Packet.zip'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in sorted(p for p in target.rglob('*') if p.is_file()):
            info=zipfile.ZipInfo(f'{target.name}/{p.relative_to(target).as_posix()}',date_time=(2026,11,12,0,0,0))
            info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=0o100644 << 16
            z.writestr(info,p.read_bytes(),compresslevel=9)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for item in manifest['files']:
            data=z.read(target.name+'/'+item['path'])
            assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256']
    result={'status':'passed','packet':str(archive.resolve()),'bytes':archive.stat().st_size,
            'sha256':digest(archive),'files':len(manifest['files'])+1,'word_counts':counts,
            'pdf_sha256':visual['pdf_sha256'],'pdf_pages':5,'manifest_and_zip_crc_verified':True}
    save(destination/'DELIVERY_RECEIPT.json',result)
    print(json.dumps(result,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--source-archive',type=Path,required=True)
    parser.add_argument('--previous-archive',type=Path,required=True)
    parser.add_argument('--destination',type=Path,required=True)
    args=parser.parse_args()
    build(args.work,args.source_archive,args.previous_archive,args.destination)
