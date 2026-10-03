import os
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "level_fixtures")
os.makedirs(d, exist_ok=True)

def w(name, s, note=""):
    if isinstance(s, str): s = s.encode()
    open(os.path.join(d, name + ".json"), "wb").write(s)
    print("%-34s %5d B  %s" % (name, len(s), note))

def wrap(angle='"angleData": [0, 180, 90]', settings='"settings": {"version": 15, "bpm": 100, "offset": 0, "hitsound": "Kick", "hitsoundVolume": 100}', actions='"actions": [{"floor": 1, "eventType": "Twirl"}, {"floor": 2, "eventType": "SetSpeed", "speedType": "Multiplier", "bpmMultiplier": 1.5}]', deco='"decorations": []'):
    return "{%s, %s, %s, %s}" % (angle, settings, actions, deco)

# ---- control ----
w("f00_control_valid", wrap(), "clean JSON control")

# ---- rule: insert ',' before '"' after } ] " digit ----
w("f01_actions_missing_comma", '{"angleData": [0, 180, 90], "settings": {"version": 15, "bpm": 100}, "actions": [{"floor": 1, "eventType": "Twirl"}{"floor": 2, "eventType": "SetSpeed", "beatsPerMinute": 300}], "decorations": []}', '[{...}{...}]  } then {')
w("f02_toplevel_missing_comma", '{"angleData": [0, 180, 90]"settings": {"bpm": 100}, "actions": []"decorations": []}', '] then "')
w("f03_digit_then_key", '{"angleData": [0, 180, 90], "settings": {"version": 15"bpm": 222.5}, "actions": [{"floor": 1, "eventType": "Twirl"}], "decorations": []}', '100 then "')
w("f04_str_then_key", '{"angleData": [0, 180, 90], "settings": {"bpm": 100, "hitsound": "Snare""hitsoundVolume": 42}, "actions": [], "decorations": []}', '".." then "')
w("f13_obj_then_key", '{"angleData": [0, 180, 90], "settings": {"bpm": 100, "position": [1, 2]}, "actions": [{"floor": 3, "eventType": "Bookmark"}]"decorations": []}', '} then "')
w("f14_action_members_missing_comma", '{"angleData": [0, 180, 90], "settings": {"bpm": 100}, "actions": [{"floor": 1"eventType": "Twirl"}, {"floor": 2"eventType": "SetSpeed", "beatsPerMinute": 250}], "decorations": []}', 'action 内部缺逗号')
w("f20_extra_nested_then_next", '{"angleData": [0, 180], "settings": {"bpm": 100}, "actions": [{"floor": 1, "eventType": "Twirl", "extra": {"a": [1, 2]}}{"floor": 2, "eventType": "Twirl"}], "decorations": []}', '} then { 在嵌套对象后')
w("f15_decorations_missing_comma", '{"angleData": [0, 180], "decorations": [{"a": 1}{"b": 2}], "settings": {"bpm": 100}, "actions": [{"floor": 1, "eventType": "Twirl"}]}', 'decorations 缺逗号 + 顺序在前')

# ---- rule: insert ',' before '{' '[' after } ] " digit ----
w("f17_nested_array_missing_comma", '{"angleData": [[0, 180][90]], "settings": {"bpm": 100}, "actions": [], "decorations": []}', '] then [')
w("f18_array_after_digit", '{"angleData": [0, 180], "settings": {"bpm": 100, "position": [1, 2]}, "actions": [{"floor": 1, "eventType": "PositionTrack", "positionOffset": [0.5, -0.25]}{"floor": 2, "eventType": "Twirl"}], "decorations": []}', '] then {')

# ---- rule: drop ',' before } ] ----
w("f05_trailing_comma_array", '{"angleData": [0, 180, 90,], "settings": {"bpm": 100}, "actions": [], "decorations": []}', '[... ,]')
w("f06_trailing_comma_object", '{"angleData": [0, 180], "settings": {"bpm": 100,}, "actions": [{"floor": 1, "eventType": "Twirl",}], "decorations": [],}', 'trailing comma in object')
w("f07_trailing_comma_ws", '{"angleData": [0, 180,\n\t   ], "settings": {"bpm": 100\n },\n "actions": [], "decorations": []}', ', 空白 ]')

# ---- rule: drop doubled ','; leading ',' after { [ ----
w("f08_double_comma", '{"angleData": [0,, 180], "settings": {"bpm": 100}, "actions": [], "decorations": []}', '[0,,180]')
w("f09_leading_comma_array", '{"angleData": [,0, 180], "settings": {"bpm": 100}, "actions": [], "decorations": []}', '[,0')
w("f10_leading_comma_object", '{"angleData": [0, 180], "settings": {, "bpm": 100}, "actions": [], "decorations": []}', '{, "bpm"')
w("f22_comma_space_close", '{"angleData": [0, 180 , , ], "settings": {"bpm": 100}, "actions": [], "decorations": []}', '[0,180 , , ]')

# ---- CR handling ----
w("f11_crlf", '{\r\n\t"angleData": [0,\r\n 180],\r\n\t"settings": {"bpm": 100},\r\n\t"actions": [{"floor": 1, "eventType": "Twirl"}],\r\n\t"decorations": []\r\n}', 'CRLF everywhere')
w("f12_cr_in_string", '{"angleData": [0, 180], "settings": {"bpm": 100, "hitsound": "Ki\rck", "levelDesc": "a\rb"}, "actions": [], "decorations": []}', 'raw CR inside strings')

# ---- other semantics ----
w("f16_pathdata_only", '{"pathData": "RRLLUD", "settings": {"bpm": 123}, "actions": [{"floor": 2, "eventType": "Twirl"}], "decorations": []}', 'no angleData, pathData path')
w("f19_all_action_types", '''{"angleData": [0, 180, 90, 270, 45, 135, 225, 315],
"settings": {"bpm": 150},
"actions": [
  {"floor": 1, "eventType": "Twirl"},
  {"eventType": "SetSpeed", "floor": 2, "speedType": "Multiplier", "bpmMultiplier": 2.0},
  {"floor": 3, "eventType": "SetSpeed", "beatsPerMinute": 400},
  {"floor": 4, "eventType": "PositionTrack", "positionOffset": [3.5, -2.25], "justThisTile": true},
  {"floor": 5, "eventType": "SetHitsound", "hitsound": "Snare", "hitsoundVolume": 55},
  {"floor": 6, "eventType": "Bookmark"},
  {"floor": 7, "eventType": "Pause", "duration": 2.5},
  {"floor": 8, "eventType": "AnimateTrack", "trackDisappearAnimation": "Scatter", "trackAnimation": "Fade", "beatsBehind": 1.5, "beatsAhead": 6.0},
  {"floor": 3, "eventType": "UnknownType", "x": 1}
],
"decorations": []}''', 'all FastAction types + unknown')
w("f24_positiontrack_variants", '''{"angleData": [0, 180, 90, 270],
"settings": {"bpm": 100},
"actions": [
  {"floor": 1, "eventType": "PositionTrack", "justThisTile": "Enabled"},
  {"floor": 2, "eventType": "PositionTrack", "justThisTile": "true", "positionOffset": [1, 1]},
  {"floor": 3, "eventType": "PositionTrack", "justThisTile": 1, "positionOffset": [-1, -1]},
  {"floor": 4, "eventType": "PositionTrack", "justThisTile": "Disabled"}
],
"decorations": []}''', 'justThisTile 各类型')
w("f25_setspeed_variants", '''{"angleData": [0, 180, 90],
"settings": {"bpm": 100},
"actions": [
  {"floor": 1, "eventType": "SetSpeed", "speedType": "Multiplier"},
  {"floor": 2, "eventType": "SetSpeed"},
  {"floor": 3, "eventType": "SetSpeed", "speedType": "BPM", "beatsPerMinute": 0}
],
"decorations": []}''', 'SetSpeed 缺字段')
w("f26_string_then_digit", '{"angleData": [0, 180], "settings": {"bpm": 100, "trackColor": "ff0000"1}, "actions": [], "decorations": []}', 'cleanJson 不修: 两边应一致(可能都失败)')
w("f27_whitespace_heavy", '{\n\t"angleData"  :  [ 0 ,\t180 ,\n90 ] ,\n\t"settings"\t:\t{\n\t\t"bpm"\t:\t100\n\t}\n,\n\t"actions"\t:\t[\n\t\t{ "floor" : 1 , "eventType" : "Twirl" }\n\t]\n,\n\t"decorations" : [ ]\n}', '大量空白(cleanJson 保留空白)')
w("f18b_sci_neg_numbers", '{"angleData": [1e2, -0.5, 3.0000000000000004e+00, 999, 555, 666, 0.1], "settings": {"bpm": 1.5e2, "offset": -12.5}, "actions": [], "decorations": []}', '科学计数/负数/999/555')
w("f23_string_escapes", '{"angleData": [0, 180], "settings": {"bpm": 100, "hitsound": "a\\"b", "levelDesc": "\\u0041\\n"}, "actions": [{"floor": 1, "eventType": "SetHitsound", "hitsound": "x\\\\y"}], "decorations": []}', '转义字符串')
w("f28_empty_arrays", '{"angleData": [], "settings": {"bpm": 100}, "actions": [], "decorations": []}', '全空')
w("f29_minimal", '{"settings": {"bpm": 200}, "angleData": [0]}', '最小')
w("f21_bom", b'\xef\xbb\xbf{"angleData": [0, 180], "settings": {"bpm": 100}, "actions": [], "decorations": []}', 'UTF-8 BOM')
w("f30_comma_then_bracket_ws", '{"angleData": [0, 180, \r\n\t], "settings": {"bpm": 100}, "actions": [\r\n], "decorations": [,]}', ', CRLF 空白 ]  + [,]')

# ---- 裸 CR 出现在字符串里：cleanJson 会删掉，快路径必须交给旧路径 ----
w("f31_cr_in_action_string", '{"angleData": [0, 180], "settings": {"bpm": 100}, "actions": [{"floor": 1, "eventType": "SetHitsound", "hitsound": "Ki\rck"}], "decorations": []}', 'action 字符串里的裸 CR')
w("f32_cr_in_pathdata", '{"pathData": "RR\rLLUD", "settings": {"bpm": 123}, "actions": [], "decorations": []}', 'pathData 里的裸 CR')

# ---- 明显非法的 token：DOM 会拒绝，快路径也应拒绝（回退旧路径后同样失败）----
w("f33_bad_literal_true", '{"angleData": [0, 180], "settings": {"bpm": 100}, "actions": [], "decorations": [True]}', 'True 不是合法 JSON')
w("f34_bad_literal_bareword", '{"angleData": [0, 180], "settings": {"bpm": 100}, "actions": [{"floor": 1, "eventType": Twirl}], "decorations": []}', '未加引号的裸词')
w("f35_bad_number_plus", '{"angleData": [0, 180], "settings": {"bpm": +100}, "actions": [], "decorations": []}', '+100 不是合法 JSON 数字')
w("f36_truncated", '{"angleData": [0, 180], "settings": {"bpm": 100}, "actions": [{"floor": 1, "eventType": "Twirl"}', '截断的文件')
