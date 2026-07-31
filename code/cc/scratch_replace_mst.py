import re

with open('src/chi_transaction_manager.cc', 'r', encoding='utf-8') as f:
    content = f.read()

content = re.sub(r'\"mst=\"\s*<<\s*([a-zA-Z0-9_\.]+)', r'"MST=" << get_mst_name(\1)', content)
content = re.sub(r'\"\s*mst=\"\s*<<\s*([a-zA-Z0-9_\.]+)', r'" MST=" << get_mst_name(\1)', content)
content = re.sub(r'mst=\"\s*<<\s*([a-zA-Z0-9_\.]+)', r'MST=" << get_mst_name(\1)', content)

with open('src/chi_transaction_manager.cc', 'w', encoding='utf-8') as f:
    f.write(content)

with open('src/transaction_manager.cc', 'r', encoding='utf-8') as f:
    content2 = f.read()

content2 = re.sub(r'\"mst=\"\s*<<\s*([a-zA-Z0-9_\.]+)', r'"MST=" << get_mst_name(\1)', content2)
content2 = re.sub(r'\"\s*mst=\"\s*<<\s*([a-zA-Z0-9_\.]+)', r'" MST=" << get_mst_name(\1)', content2)
content2 = re.sub(r'mst=\"\s*<<\s*([a-zA-Z0-9_\.]+)', r'MST=" << get_mst_name(\1)', content2)

with open('src/transaction_manager.cc', 'w', encoding='utf-8') as f:
    f.write(content2)

