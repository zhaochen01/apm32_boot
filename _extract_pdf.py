# -*- coding: utf-8 -*-
import PyPDF2
import os

base = r'E:\Docs\Quectel\RG200_RM500'
for f in os.listdir(base):
    if f.endswith('.pdf'):
        pdf_path = os.path.join(base, f)
        print(f"Found: {f}")

reader = PyPDF2.PdfReader(pdf_path)
if reader.is_encrypted:
    reader.decrypt('')

print(f"Total PDF pages: {len(reader.pages)}")

targets = ['usbnet', 'usbcfg', 'usbmode', 'usbid', 'usbreset']

for i, page in enumerate(reader.pages):
    text = page.extract_text()
    if text:
        found = False
        for s in targets:
            if ('QCFG="' + s + '"') in text:
                found = True
                break
        if found:
            print(f'\n===== PDF Page {i+1} =====')
            print(text[:5000])
            print('---END OF PAGE---')
