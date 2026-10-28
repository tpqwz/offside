export type LangId = "c" | "java" | "javascript" | "pascal";

export type TokenKind =
  | "ident"
  | "keyword"
  | "number"
  | "string"
  | "char"
  | "template"
  | "regex"
  | "comment"
  | "directive"
  | "punct";

export interface Tok {
  kind: TokenKind;
  text: string;
  /** 1-based line of the first character. */
  line: number;
  /** 1-based column of the first character. Tabs are stops of 8. */
  col: number;
  start: number;
  end: number;
}

export interface LayoutNote {
  line: number;
  message: string;
}

export interface LayoutEvent {
  line: number;
  message: string;
}

export interface OutSegment {
  text: string;
  inserted: boolean;
}

export interface OutLine {
  segments: OutSegment[];
  /** Input line this output mostly came from. */
  sourceLine: number;
}

export interface LayoutResult {
  language: LangId;
  lines: OutLine[];
  code: string;
  events: LayoutEvent[];
  notes: LayoutNote[];
}

export interface Example {
  id: string;
  label: string;
  source: string;
}

export interface Profile {
  id: LangId;
  label: string;
  filename: string;
  keywords: ReadonlySet<string>;
  preprocessor: boolean;
  templates: boolean;
  regex: boolean;
  /** Close this kind of header with `};` when the header has no `(`. */
  semiClose: ReadonlySet<string>;
  policy: {
    cue: string;
    terminator: string;
    semiClose: string;
    transparent: string;
  };
  examples: Example[];
}
