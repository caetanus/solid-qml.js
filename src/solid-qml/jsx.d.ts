import type { JSX } from "solid-js";

declare module "solid-js" {
  namespace JSX {
    interface IntrinsicElements {
      Window: WindowProps;
      div: QmlDivProps;
      span: QmlTextProps;
      text: QmlTextProps;
      hr: QmlHrProps;
      button: QmlButtonProps;
      combo: QmlControlProps;
      radio: QmlControlProps;
      check: QmlControlProps;
    }
  }
}

type BaseQmlProps = {
  id?: string;
  class?: string;
  className?: string;
  children?: JSX.Element;
};

type WindowProps = BaseQmlProps & {
  title?: string;
  width?: number;
  height?: number;
  visible?: boolean;
};

type QmlDivProps = BaseQmlProps &
  Omit<JSX.HTMLAttributes<HTMLDivElement>, "id" | "class" | "className">;

type QmlTextProps = BaseQmlProps &
  Omit<JSX.HTMLAttributes<HTMLElement>, "id" | "class" | "className">;

type QmlHrProps = Omit<BaseQmlProps, "children"> &
  Omit<JSX.HTMLAttributes<HTMLHRElement>, "id" | "class" | "className" | "children">;

type QmlButtonProps = BaseQmlProps &
  Omit<JSX.ButtonHTMLAttributes<HTMLButtonElement>, "id" | "class" | "className"> & {
    onClick?: JSX.EventHandlerUnion<HTMLButtonElement, MouseEvent>;
    onClicked?: JSX.EventHandlerUnion<HTMLButtonElement, MouseEvent>;
  };

type QmlControlProps = BaseQmlProps & Record<string, unknown>;
